/*
 * Aegir's director: the root task.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * director is the first Aegir process, and the only one that begins with the
 * machine's authority (specs/director.md). What it does here:
 *
 *   - takes the machine's memory and capability slots from the bootinfo, and
 *     proves it can retype an object and map it;
 *   - reads its own initrd -- a flat filesystem with no paths, where the entry
 *     names are the service identities (specs/services.md) -- and lists it;
 *   - parses the boot manifest and reports what it declares.
 *
 * What it does not do yet: create a process. That is the next step, and the
 * smoke client it will start is already packed into the initrd beside the
 * manifest. Director says nothing is well until it can prove it, so the boot
 * marker only appears when every check above passed.
 */

#include <aegir/debug.h>
#include <aegir/devtree.h>
#include <aegir/manifest.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/vspace.h>
#include <aegir/spawn/initrd.h>
#include <aegir/spawn/process.h>
#include <aegir/spawn/process.h>

#include <sel4/sel4.h>

#include "services.h"

/* util_libs' cpio header has no extern "C" guard, so from C++ its prototypes
 * would be mangled and the link would fail on names the library does not define
 * -- the same C/C++ boundary as sel4runtime's header, handled the same way. */
extern "C" {
#include <cpio/cpio.h>
}

extern "C" {
/* Our initrd: the archive the build packs into this image (specs/services.md).
 * The ELF loader's own archive is a different one and not readable here. */
extern char _cpio_archive[];
extern char _cpio_archive_end[];

/* sel4runtime's public header is C-only -- sel4runtime/stdint.h uses
 * _Static_assert, which C++ rejects
 * (projects/sel4runtime/include/sel4runtime/stdint.h:15-19) -- so director
 * declares the one thing it needs from the runtime rather than including it.
 * The same class of boundary as sel4/assert.h's __assert_fail, which
 * libs/aegir-runtime/src/assert.cc exists to cross. */
seL4_BootInfo *sel4runtime_bootinfo(void);
}

namespace {

using aegir::director::Boot;
using aegir::director::Services;
using aegir::director::Started;
using aegir::director::Supervised;
using aegir::director::Supervisor;

/* The name director looks its manifest up by: the flat name the archive uses,
 * which is the file's basename. */
constexpr char const kManifestEntry[] = "services.manifest";
constexpr uint32_t kManifestEntryLength = sizeof(kManifestEntry) - 1;

int failures = 0;

void write(char const *text) noexcept
{
    aegir::debug_write(text);
}

void number(uint64_t value) noexcept
{
    aegir::debug_write_unsigned(value);
}

void heading(char const *text) noexcept
{
    write("\n");
    write(text);
    write("\n");
}

/* Sizes here run from kilobytes to hundreds of gigabytes -- QEMU's virt machine
 * declares enormous device ranges, and the device manager will care about them --
 * so the unit is chosen rather than fixed. */
void report_size(uint64_t bytes) noexcept
{
    if (bytes >= (1ull << 30)) {
        /* Fractional, because whole-GiB truncation turns 2044 MiB into "1 GiB"
         * and 3069 MiB into "2 GiB" -- a report that rounds by a gigabyte is
         * worse than no report. */
        number(bytes >> 30);
        write(".");
        uint64_t hundredths = ((bytes & ((1ull << 30) - 1)) * 100) >> 30;
        if (hundredths < 10) {
            write("0");
        }
        number(hundredths);
        write(" GiB");
    } else if (bytes >= (1ull << 20)) {
        number(bytes >> 20);
        write(" MiB");
    } else if (bytes >= (1ull << 10)) {
        number(bytes >> 10);
        write(" KiB");
    } else {
        number(bytes);
        write(" bytes");
    }
}

void problem(char const *what) noexcept
{
    write("  FAIL ");
    write(what);
    write("\n");
    ++failures;
}

void write_name(char const *name, unsigned length) noexcept
{
    for (unsigned i = 0; i < length; ++i) {
        seL4_DebugPutChar(name[i]);
    }
}

/** Map the device tree the firmware left us, and say where it is.
 *
 *  The blob lives in the extra bootinfo pages, whose frame capabilities the kernel
 *  put in our CSpace, and it is read in place: those pages are already mapped
 *  ("RISCVPageMap: attempting to map frame into multiple addresses"), so they are
 *  moved into the scratch window, which only ever hands out the next page -- which
 *  is also what makes them contiguous to walk. Where the blob *starts* is not
 *  something we assume: the magic says so.
 *
 *  Director does not read the tree's contents. It maps it, checks that it is one,
 *  and hands it to the device manager, which is the service that owns what the
 *  machine is (specs/services.md).
 *
 *  Nothing is unmapped afterwards: the blob stays mapped, because the bytes handed
 *  on are those bytes. */
unsigned map_device_tree(seL4_BootInfo const *bootinfo, aegir::mem::Scratch *scratch,
                         void const **blob_out, uint32_t *bytes_out) noexcept {
    *blob_out = nullptr;
    *bytes_out = 0;

    uint32_t const pages =
        static_cast<uint32_t>(bootinfo->extraBIPages.end - bootinfo->extraBIPages.start);
    if (pages == 0) {
        aegir::debug_write("  FAIL the kernel gave us no pages for a device tree\n");
        return 1;
    }

    uint8_t const *blob = nullptr;
    uint64_t mapped = 0;
    for (seL4_CPtr cap = bootinfo->extraBIPages.start; cap < bootinfo->extraBIPages.end; ++cap) {
        seL4_RISCV_Page_Unmap(cap);
        auto *page = static_cast<uint8_t *>(scratch->map(cap));
        if (page == nullptr) {
            aegir::debug_write("  FAIL the device tree could not be mapped\n");
            return 1;
        }
        if (blob == nullptr) {
            blob = page;
        }
        mapped += 4096;
    }

    uint64_t offset = 0;
    aegir::devtree::Tree tree;
    bool adopted = false;
    while (offset + 40 <= mapped) {
        if (blob[offset] == 0xd0 && blob[offset + 1] == 0x0d && blob[offset + 2] == 0xfe &&
            blob[offset + 3] == 0xed) {
            if (tree.adopt(blob + offset, mapped - offset)) {
                adopted = true;
                break;
            }
        }
        offset += 4;
    }
    if (!adopted) {
        aegir::debug_write("  FAIL no device tree in the bootinfo pages\n");
        return 1;
    }

    aegir::debug_write("  device tree: version ");
    aegir::debug_write_unsigned(tree.version());
    aegir::debug_write(", ");
    aegir::debug_write_unsigned(tree.total_size() / 1024);
    aegir::debug_write(" KiB, ");
    aegir::debug_write_unsigned(tree.reserved_entries());
    aegir::debug_write(" reserved regions, handed to the device manager\n");

    *blob_out = blob + offset;
    *bytes_out = tree.total_size();
    return 0;
}

/** A platform device the tree names by compatible rather than by a virtio id:
 *  where its registers are, and what the tree calls it. The compatible string
 *  points into the blob, which is mapped for director's lifetime. */
struct PlatformEntry {
    uint64_t base;
    uint64_t size;
    char const *compatible;
    uint32_t compatible_length;
};

/** Collect every node with a register window whose transport is not virtio.
 *  The survey hands these out by compatible, which is how a platform device
 *  without an id reaches the one service that claims it (specs/services.md). */
class PlatformCollector : public aegir::devtree::Tree::Visitor {
public:
    bool device(aegir::devtree::Device const &device) override {
        if (!device.has_region) {
            return true;
        }
        static char const virtio[] = "virtio,mmio";
        if (device.compatible_length == sizeof(virtio) - 1) {
            bool same = true;
            for (uint32_t i = 0; i < device.compatible_length && same; ++i) {
                same = device.compatible[i] == virtio[i];
            }
            if (same) {
                return true;
            }
        }
        if (list != nullptr && count < capacity) {
            list[count] = PlatformEntry{device.base, device.size, device.compatible,
                                        device.compatible_length};
        }
        ++count;
        return true;
    }

    PlatformEntry *list = nullptr;
    uint32_t capacity = 0;
    uint32_t count = 0;
};

/* The platform device whose register window covers an address, or null. */
PlatformEntry const *platform_at(PlatformEntry const *list, uint32_t count,
                                 uint64_t address) noexcept
{
    for (uint32_t i = 0; i < count; ++i) {
        if (address >= list[i].base && address < list[i].base + list[i].size) {
            return &list[i];
        }
    }
    return nullptr;
}

/* A copy of a device frame cap, made before any service maps it: one frame cap
 * pins to the VSpace it is first mapped into, so a second service that maps the
 * same device needs its own copy, and a copy taken before the first mapping
 * carries no ASID yet (kernel/src/arch/riscv/kernel/vspace.c:869-878). Zero
 * when the copy fails. */
seL4_CPtr copy_device_frame(aegir::mem::Allocator &allocator, seL4_CPtr frame) noexcept
{
    seL4_CPtr const slot = allocator.alloc_slot();
    if (slot == 0 ||
        seL4_CNode_Copy(aegir::bootstrap::kSlotOwnCNode, slot,
                        aegir::bootstrap::kCNodeBits, aegir::bootstrap::kSlotOwnCNode,
                        frame, aegir::bootstrap::kCNodeBits, seL4_AllRights) !=
            seL4_NoError) {
        return 0;
    }
    return slot;
}

/** Take the frame of each platform device a service claims by compatible.
 *  Only claimed devices are reached: a window is taken up to the device's
 *  page from the device untyped that covers it, and a device no service names
 *  is left alone (its untyped may be far below it, and reaching a page in the
 *  middle of an untyped costs every page before it). A device already in the
 *  bus -- the survey recorded it inside the transports' untyped -- is left as
 *  it is. */
unsigned survey_claimed_platform_devices(seL4_BootInfo const *bootinfo,
                                         aegir::mem::Allocator &allocator,
                                         PlatformEntry const *platform, uint32_t platform_count,
                                         aegir::manifest::Manifest const &manifest,
                                         aegir::director::Device *found, uint32_t capacity,
                                         uint32_t *device_count) noexcept
{
    for (uint32_t e = 0; e < manifest.size(); ++e) {
        aegir::manifest::Entry const &entry = manifest[e];
        if (entry.device.length == 0) {
            continue;
        }
        for (uint32_t p = 0; p < platform_count; ++p) {
            if (platform[p].compatible_length != entry.device.length) {
                continue;
            }
            bool same = true;
            for (uint32_t i = 0; i < entry.device.length && same; ++i) {
                same = platform[p].compatible[i] == entry.device.data[i];
            }
            if (!same) {
                continue;
            }
            uint64_t const base = platform[p].base;
            bool already = false;
            for (uint32_t d = 0; d < *device_count && !already; ++d) {
                already = found[d].address == base;
            }
            if (already) {
                break; /* another service claimed the same one first */
            }
            bool reached = false;
            seL4_Word const count = bootinfo->untyped.end - bootinfo->untyped.start;
            for (seL4_Word i = 0; i < count && !reached; ++i) {
                seL4_UntypedDesc const &desc = bootinfo->untypedList[i];
                if (desc.isDevice == 0) {
                    continue;
                }
                uint64_t const span = 1ull << desc.sizeBits;
                if (base < desc.paddr || base >= desc.paddr + span) {
                    continue;
                }
                uint64_t const untyped = desc.paddr;
                unsigned const pages =
                    static_cast<unsigned>(((base - untyped) >> seL4_PageBits) + 1);
                seL4_Error error = seL4_NoError;
                seL4_CPtr window_first = 0;
                if (!allocator.device_window(untyped, pages, &window_first, &error)) {
                    aegir::debug_write("  FAIL a claimed device's window could not be taken at ");
                    aegir::debug_write_hex(base);
                    aegir::debug_write(" (seL4 error ");
                    aegir::debug_write_unsigned(static_cast<uint64_t>(error));
                    aegir::debug_write(")\n");
                    return 1;
                }
                seL4_CPtr const slot = window_first + ((base - untyped) >> seL4_PageBits);
                aegir::debug_write("  platform device at ");
                aegir::debug_write_hex(base);
                aegir::debug_write(": ");
                aegir::debug_write(platform[p].compatible, platform[p].compatible_length);
                aegir::debug_write(", frame at cap ");
                aegir::debug_write_unsigned(slot);
                aegir::debug_write("\n");
                if (*device_count < capacity) {
                    found[*device_count] = aegir::director::Device{
                        base, 0, slot, platform[p].compatible, platform[p].compatible_length,
                        copy_device_frame(allocator, slot)};
                }
                ++(*device_count);
                reached = true;
            }
            if (!reached) {
                aegir::debug_write("  FAIL no device untyped covers the claimed device at ");
                aegir::debug_write_hex(base);
                aegir::debug_write("\n");
                return 1;
            }
            break;
        }
    }
    return 0;
}

/** Report which of the machine's transports has something behind it.
 *
 *  This is the first content of the bus map: a device id is what says whether a
 *  transport is busy, and which transport is which is what a driver needs to be
 *  told. An empty transport answers the magic and device id zero; a busy one answers
 *  the magic and its own id (virtio 1.x, 4.2.2).
 *
 *  The pages are walked in one ascending pass from the untyped's base, each retyped
 *  into its own slot and kept: a retype carves from the untyped's cursor, and a
 *  frame that is dropped goes back to it, so the pages before the first transport
 *  have to be taken anyway and taking them costs one retype each. Only the pages
 *  between the two ends the tree names are read -- every page in that span is a
 *  transport, which is what makes reading them safe. */
unsigned survey_devices(seL4_BootInfo const *bootinfo, aegir::mem::Allocator &allocator,
                        aegir::mem::Scratch &scratch, uint64_t untyped_base, uint64_t first,
                        uint64_t last, PlatformEntry const *platform,
                        uint32_t platform_count, aegir::director::Device *found,
                        uint32_t capacity, uint32_t *device_count) noexcept {
    *device_count = 0;
    seL4_Word const count = bootinfo->untyped.end - bootinfo->untyped.start;
    for (seL4_Word i = 0; i < count; ++i) {
        seL4_UntypedDesc const &desc = bootinfo->untypedList[i];
        if (desc.isDevice == 0 || desc.paddr != untyped_base) {
            continue;
        }
        uint64_t const span = 1ull << desc.sizeBits;
        uint64_t const end = last + (1ull << seL4_PageBits);
        if (end > untyped_base + span) {
            aegir::debug_write("  FAIL the transports run past their untyped\n");
            return 1;
        }
        unsigned const pages = static_cast<unsigned>((end - untyped_base) >> seL4_PageBits);
        unsigned busy = 0;
        /* The window comes from the library in one call, and the first capability it
         * hands back is what the loop indexes from -- so it is read *once*, into a
         * local. The survey used to overwrite it with each busy page's slot, which is
         * how `first + page` ended up naming a slot nobody had retyped. */
        seL4_Error window_error = seL4_NoError;
        seL4_CPtr window_first = 0;
        if (!allocator.device_window(untyped_base, pages, &window_first, &window_error)) {
            aegir::debug_write("  FAIL the device window could not be taken (seL4 error ");
            aegir::debug_write_unsigned(static_cast<uint64_t>(window_error));
            aegir::debug_write(")\n");
            return 1;
        }
        for (unsigned page = 0; page < pages; ++page) {
            uint64_t const address = untyped_base + (static_cast<uint64_t>(page) << seL4_PageBits);
            seL4_CPtr const slot = window_first + page;
            if (address < first) {
                /* A page before the transports: taken to advance the cursor.
                 * The tree names no virtio transport here, but it may name a
                 * platform device -- the RTC sits just above the UART -- and
                 * that device is recorded with the name the tree gives it,
                 * which is how it reaches the service that claims it without
                 * an id (specs/services.md). */
                PlatformEntry const *named =
                    platform_at(platform, platform_count, address);
                if (named != nullptr) {
                    aegir::debug_write("  platform device at ");
                    aegir::debug_write_hex(address);
                    aegir::debug_write(": ");
                    aegir::debug_write(named->compatible, named->compatible_length);
                    aegir::debug_write("\n");
                    if (*device_count < capacity) {
                        found[*device_count] = aegir::director::Device{
                            address, 0, slot, named->compatible, named->compatible_length,
                            copy_device_frame(allocator, slot)};
                    }
                    ++(*device_count);
                }
                continue;
            }
            auto *registers = static_cast<volatile uint32_t *>(scratch.map(slot));
            if (registers == nullptr) {
                aegir::debug_write("  FAIL the transport could not be mapped: cap ");
                aegir::debug_write_unsigned(slot);
                aegir::debug_write(" of ");
                aegir::debug_write_unsigned(window_first);
                aegir::debug_write(" + ");
                aegir::debug_write_unsigned(page);
                aegir::debug_write("\n");
                return 1;
            }
            uint32_t const magic = registers[0x00 / 4];
            uint32_t const device_id = registers[0x08 / 4];
            /* Unmapped as soon as it has been read. A frame capability that has been
             * mapped stays mapped, and `RISCVPageUnmap` clears the address space on the
             * capability it is invoked on and no other
             * (kernel/src/arch/riscv/kernel/vspace.c, `performPageInvocationUnmap`), so
             * leaving these mapped is how a frame ends up still belonging to an address
             * space when it is handed to a service (specs/services.md). */
            scratch.unmap(slot);
            if (device_id == 0) {
                continue;
            }
            ++busy;
            /* The survey's list: one entry per device the machine has behind a transport, in
             * the order the tree names them. A service is given the device its section names
             * by id, and identical transports answer identically to every register read, so
             * the id and the position together are what say which device it holds
             * (specs/services.md). The two outputs below keep reporting the last device for
             * the report that follows; the list is what a service is given. */
            if (*device_count < capacity) {
                found[*device_count] = aegir::director::Device{address, device_id, slot,
                                                               nullptr, 0, 0};
            }
            ++(*device_count);
            aegir::debug_write("  device at ");
            aegir::debug_write_hex(address);
            aegir::debug_write(": magic ");
            aegir::debug_write_hex(magic);
            aegir::debug_write(", device id ");
            aegir::debug_write_unsigned(device_id);
            aegir::debug_write("\n");
        }
        aegir::debug_write("  transports: ");
        aegir::debug_write_unsigned(busy);
        aegir::debug_write(" of ");
        aegir::debug_write_unsigned(pages);
        aegir::debug_write(" device pages have a device behind them\n");
        return 0;
    }
    aegir::debug_write("  FAIL no device untyped at ");
    aegir::debug_write_hex(untyped_base);
    aegir::debug_write("\n");
    return 1;
}

/* One is enough to find out whether a device can be reached at all, before a
 *  service is given
 *  find out whether a device can be reached at all, before a service is given
 *  one. */
class FirstVirtioTransport : public aegir::devtree::Tree::Visitor {
public:
    bool device(aegir::devtree::Device const &device) override {
        if (!device.has_region) {
            return true;
        }
        static char const wanted[] = "virtio,mmio";
        uint32_t const length = device.compatible_length;
        if (length != sizeof(wanted) - 1) {
            return true;
        }
        for (uint32_t i = 0; i < length; ++i) {
            if (device.compatible[i] != wanted[i]) {
                return true;
            }
        }
        /* The tree lists the transports in descending order and QEMU gives devices
         * to them from the bottom up, so the transport with a device behind it is
         * the lowest address: walk the whole tree and keep that one. */
        if (!found || device.base < base) {
            base = device.base;
            size = device.size;
            interrupt = device.interrupt;
        }
        if (device.base > highest) {
            highest = device.base;
        }
        found = true;
        return true;
    }

    bool found = false;
    uint64_t highest = 0;
    uint64_t base = 0;
    uint64_t size = 0;
    uint32_t interrupt = 0;
};

/** Where a device's registers live, as memory the kernel calls device.
 *
 *  This is the question every driver's first line depends on. Device memory is
 *  untyped like any other, marked as device, and a device frame can only be
 *  retyped from one -- but `Untyped_Retype` takes no interior offset
 *  (kernel/src/object/untyped.c, `decodeUntypedInvocation`: type, sizeBits,
 *  nodeIndex, nodeDepth, nodeOffset, nodeWindow), so it carves from the untyped's
 *  own free position. Reaching a device therefore means knowing how far into its
 *  untyped the device sits, which is what this reports: the untyped that covers
 *  the address, and the page's position within it. */
unsigned report_device_memory(seL4_BootInfo const *bootinfo, void const *blob, uint32_t bytes,
                             uint64_t *untyped_out, uint64_t *first_out,
                             uint64_t *last_out) noexcept {
    if (blob == nullptr) {
        return 0;
    }
    aegir::devtree::Tree tree;
    if (!tree.adopt(blob, bytes)) {
        aegir::debug_write("  FAIL the blob handed to the device manager is not a tree\n");
        return 1;
    }
    FirstVirtioTransport first;
    if (!tree.walk(first) || !first.found) {
        aegir::debug_write("  FAIL the device tree names no virtio transport\n");
        return 1;
    }

    *first_out = first.base;
    *last_out = first.highest;
    aegir::debug_write("  device memory: transports from ");
    aegir::debug_write_hex(first.base);
    aegir::debug_write(" to ");
    aegir::debug_write_hex(first.highest);
    aegir::debug_write(", which the tree names first and last -- which one has a device behind\
it is what the survey below says, not the order the tree lists them in\n");

    seL4_Word const count = bootinfo->untyped.end - bootinfo->untyped.start;
    for (seL4_Word i = 0; i < count; ++i) {
        seL4_UntypedDesc const &desc = bootinfo->untypedList[i];
        if (desc.isDevice == 0) {
            continue;
        }
        uint64_t const base = desc.paddr;
        uint64_t const span = 1ull << desc.sizeBits;
        if (first.base < base || first.base - base >= span) {
            continue;
        }
        *untyped_out = base;
        aegir::debug_write("    covered by device untyped ");
        aegir::debug_write_unsigned(i);
        aegir::debug_write(" at ");
        aegir::debug_write_hex(base);
        aegir::debug_write(" of 2^");
        aegir::debug_write_unsigned(desc.sizeBits);
        aegir::debug_write(" bytes; the device page is object ");
        aegir::debug_write_unsigned(static_cast<uint64_t>(first.base - base) >> seL4_PageBits);
        aegir::debug_write(" of it\n");
        return 0;
    }
    aegir::debug_write("  FAIL no device untyped covers that address\n");
    return 1;
}

void report_bootinfo(seL4_BootInfo const *bootinfo) noexcept
{
    heading("the machine");
    write("  cnode size: 2^");
    number(bootinfo->initThreadCNodeSizeBits);
    write(" slots; ");
    number(bootinfo->empty.end - bootinfo->empty.start);
    write(" free for us\n");
    write("  untyped caps: ");
    number(bootinfo->untyped.end - bootinfo->untyped.start);
    write("; ipc buffer at ");
    aegir::debug_write_hex(reinterpret_cast<uintptr_t>(bootinfo->ipcBuffer));
    write("\n");
}

/* Retype a frame, map it somewhere we can write, write a pattern and read it
 * back. If this passes, the spawn path's two hardest prerequisites -- getting
 * an object out of untyped memory, and having an address space to fill it
 * through -- both work. */
bool self_test(aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch,
               aegir::mem::Account &account) noexcept
{
    seL4_Error error = seL4_NoError;
    seL4_CPtr frame = allocator.alloc_object(seL4_RISCV_4K_Page, seL4_PageBits, account, &error);
    if (frame == 0) {
        problem("could not retype a page out of untyped memory");
        return false;
    }
    void *page = scratch.map(frame);
    if (page == nullptr) {
        problem("could not map a page into our own address space");
        return false;
    }
    auto *bytes = static_cast<volatile uint64_t *>(page);
    for (unsigned i = 0; i < 512; ++i) {
        bytes[i] = 0xa11a6e7ull * (i + 1);
    }
    for (unsigned i = 0; i < 512; ++i) {
        if (bytes[i] != 0xa11a6e7ull * (i + 1)) {
            scratch.unmap(frame);
            problem("a page we wrote did not read back");
            return false;
        }
    }
    scratch.unmap(frame);
    return true;
}

/* List the initrd, and hand back the manifest entry by name. Two entries with
 * the same name would shadow silently -- cpio_get_file returns the first match
 * -- so a duplicate is a boot failure rather than a surprise later
 * (specs/services.md). */
bool read_initrd(aegir::spawn::Initrd const &initrd, char const *&manifest,
                 unsigned long &manifest_size) noexcept
{
    if (!initrd.valid()) {
        problem("the initrd could not be read");
        return false;
    }

    heading("initrd (flat: names are identities, there are no paths)");
    for (unsigned i = 0; i < initrd.entries(); ++i) {
        unsigned name_size = 0;
        char const *entry_name = initrd.name(i, &name_size);
        if (entry_name == nullptr) {
            problem("an initrd entry could not be read");
            return false;
        }
        write("  ");
        write_name(entry_name, name_size);
        write("\n");
    }
    /* Two entries sharing a name would shadow silently: a lookup returns the
     * first match (specs/services.md). */
    if (initrd.has_duplicate_names()) {
        problem("two initrd entries share a name, so one of them shadows the other");
        return false;
    }

    uint64_t size = 0;
    void const *found = initrd.find(kManifestEntry, kManifestEntryLength, &size);
    if (found == nullptr) {
        write("  FAIL no entry named ");
        write(kManifestEntry);
        write("\n");
        ++failures;
        return false;
    }
    manifest = static_cast<char const *>(found);
    manifest_size = static_cast<unsigned long>(size);
    return true;
}

void report_manifest(aegir::manifest::Manifest const &manifest) noexcept
{
    using aegir::manifest::Authority;
    using aegir::manifest::Entry;

    heading("boot manifest");
    write("  ");
    number(manifest.size());
    write(" service(s) declared\n");
    for (uint32_t i = 0; i < manifest.size(); ++i) {
        Entry const &entry = manifest[i];
        write("  ");
        write_name(entry.name.data, entry.name.length);
        write(": binary ");
        write_name(entry.binary.data, entry.binary.length);
        write(", authority ");
        write(entry.authority == Authority::System ? "system" : "user");
        write(", account ");
        write_name(entry.account.data, entry.account.length);
        if (entry.memory_kib > 0) {
            write(", memory ");
            number(entry.memory_kib);
            write(" KiB");
        }
        if (entry.delegate_mib > 0) {
            write(", delegation ");
            number(entry.delegate_mib);
            write(" MiB");
        }
        write("\n");
    }
}

/* Create the boot set and wait for it to say it is ready. Supervision is the
 * spawner's other half (specs/director.md): every process gets a notification,
 * and a service that never signals is a boot that does not finish -- which is the
 * honest outcome while there is no timer to time out on. */
bool boot_services(aegir::spawn::Initrd const &initrd, aegir::manifest::Manifest const &manifest,
                   aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch,
                   aegir::mem::Arena &arena, aegir::mem::Account &account,
                   void const *devices, uint32_t devices_bytes,
                   aegir::director::Device const *bus, uint32_t bus_count,
                   aegir::spawn::PortGrant const *extra, uint32_t extra_count) noexcept
{
    auto *started =
        static_cast<Started *>(arena.allocate(sizeof(Started) * (manifest.size() + 1)));
    if (started == nullptr) {
        problem("no memory for the boot set's records");
        return false;
    }

    Services services(allocator, scratch, arena, initrd);
    if (!services.prepare(account)) {
        problem("no memory for the shared fault endpoint");
        return false;
    }

    /* The supervisor starts before the first service, because a service that
     * faults before anyone is listening for faults is a service whose death
     * nobody sees (specs/director.md). */
    auto *supervised = static_cast<Supervised *>(
        arena.allocate(sizeof(Supervised) * (manifest.size() + 1)));
    if (supervised == nullptr) {
        problem("no memory for the supervisor's records");
        return false;
    }
    Supervisor supervisor(allocator, scratch, arena);
    if (!supervisor.start(services.fault_endpoint(), supervised, manifest.size(), account)) {
        problem(supervisor.problem());
        return false;
    }

    Boot boot{};
    services.boot(manifest, account, started, boot, &supervisor, devices, devices_bytes, bus,
                  bus_count, extra, extra_count);

    heading("boot set");
    write("  ");
    number(boot.declared);
    write(" declared, ");
    number(boot.started);
    write(" started\n");
    write("  ports: ");
    number(services.graph().port_count());
    write("\n");
    for (unsigned i = 0; i < services.graph().port_count(); ++i) {
        uint32_t name_length = 0;
        char const *port_name = services.graph().port_name(i, &name_length);
        write("    ");
        write_name(port_name, name_length);
        write(": owned by ");
        uint32_t owner_length = 0;
        char const *owner = manifest[services.graph().port_owner(i)].name.data;
        owner_length = manifest[services.graph().port_owner(i)].name.length;
        write_name(owner, owner_length);
        write("\n");
    }
    write("  order: ");
    for (unsigned step = 0; step < boot.started; ++step) {
        /* The graph's order is what services were created in; the records keep
         * the names as they started. */
        if (step > 0) {
            write(", ");
        }
        write_name(started[step].name, started[step].name_length);
    }
    write("\n");
    for (unsigned i = 0; i < boot.started; ++i) {
        write("  ");
        write_name(started[i].name, started[i].name_length);
        write(" running at ");
        aegir::debug_write_hex(started[i].entry);
        write(", badge ");
        number(started[i].badge);
        write("\n");
    }
    if (boot.problem[0] != '\0') {
        problem(boot.problem);
        if (boot.detail != nullptr && boot.detail[0] != '\0') {
            write("  detail: ");
            write(boot.detail);
            write(" (seL4 error ");
            number(boot.error);
            write(")\n");
        }
        write("  allocator: ");
        number(allocator.untyped_free());
        write(" untyped free, largest 2^");
        number(allocator.largest_free_bits());
        write("; last request 2^");
        number(allocator.last_request_bits());
        write(" from a 2^");
        number(allocator.last_candidate_bits());
        write(" candidate; slots ");
        number(allocator.slots_used());
        write(" of ");
        number(allocator.slots_total());
        write("\n");
        return false;
    }
    if (boot.started == 0) {
        problem("the manifest declares no service to start");
        return false;
    }

    /* Wait for each service to report ready -- or for its supervisor to say it
     * died. The badge is what tells the two apart: a service signals with the
     * capability it was given, which carries its own badge, and a death notice
     * carries director's (zero), which no service can produce for itself. */
    unsigned ready = 0;
    unsigned faulted = 0;
    for (unsigned i = 0; i < boot.started; ++i) {
        seL4_Word badge = 0;
        seL4_Wait(started[i].supervision, &badge);
        write("  ");
        write_name(started[i].name, started[i].name_length);
        if (badge == started[i].badge) {
            write(" ready\n");
            ++ready;
        } else {
            write(" did not report ready: it faulted\n");
            ++faulted;
        }
    }
    write("  ");
    number(ready);
    write(" ready, ");
    number(faulted);
    write(" faulted\n");
    return true;
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    write("\nAegir: director online\n");

    seL4_BootInfo *bootinfo = sel4runtime_bootinfo();
    if (bootinfo == nullptr) {
        problem("no bootinfo: this is not the root task");
        write("\nAEGIR_BOOT_INCOMPLETE\n");
        aegir::halt();
    }
    report_bootinfo(bootinfo);

    /* Static, not local: the allocator carries the node pool and the scratch
     * its bookkeeping, tens of kilobytes each, and the root task's stack is
     * 16 KiB -- a local would run off it into the image (specs/userland.md:
     * anything that large belongs in static storage). */
    static aegir::mem::Allocator allocator(bootinfo);
    /* The node pool, provided and sized from the grant: the buddy tree over 2 GiB
     * is far larger than a service's, and the root task's image is not mapped by a
     * spawner, so a few megabytes of `.bss` here is cheap (specs/allocator.md). */
    static uint8_t director_nodes[4u << 20];
    allocator.adopt_nodes(director_nodes, sizeof(director_nodes));
    if (!allocator.initialise()) {
        problem("the allocator could not take the machine's memory");
    }

    static aegir::mem::Scratch scratch(bootinfo);
    if (!scratch.initialise(&allocator)) {
        problem("the address space window could not be worked out");
    }

    /* The device tree, before anything is spawned: it is the only place Aegir
     * learns what the machine is, and it is handed to the device manager
     * (specs/services.md). */
    void const *device_tree = nullptr;
    uint32_t device_tree_bytes = 0;
    failures += map_device_tree(bootinfo, &scratch, &device_tree, &device_tree_bytes);
    uint64_t device_untyped = 0;
    uint64_t first_transport = 0;
    uint64_t last_transport = 0;
    /* Everything boot allocates is charged to the system account (specs/authority.md), and
     * its capacity grows on demand; there is no ceiling chosen here. Both the account and
     * the arena are built here, above the survey, because the survey's list of devices
     * lives in the arena. */
    aegir::mem::Account system{"system", 0, 0, 0};
    aegir::mem::Arena arena(allocator, scratch, system);
    failures += report_device_memory(bootinfo, device_tree, device_tree_bytes, &device_untyped,
                                    &first_transport, &last_transport);
    /* The platform devices: every node the tree names with a register window
     * that is not a virtio transport. The survey records their frames (they
     * share the device untyped the transports sit in), and a service claims
     * one by the tree's name for it -- no id, because a platform device has
     * none (specs/services.md). */
    PlatformEntry *platform = nullptr;
    uint32_t platform_count = 0;
    {
        aegir::devtree::Tree tree;
        if (device_tree != nullptr && device_tree_bytes != 0 &&
            tree.adopt(device_tree, device_tree_bytes)) {
            PlatformCollector counter;
            if (tree.walk(counter)) {
                uint32_t const room = counter.count != 0 ? counter.count : 1;
                platform = static_cast<PlatformEntry *>(
                    arena.allocate(sizeof(PlatformEntry) * room));
                PlatformCollector fill;
                fill.list = platform;
                fill.capacity = counter.count;
                static_cast<void>(tree.walk(fill));
                platform_count = counter.count;
            }
        }
    }
    /* One entry per device the machine has behind a transport and per platform
     * device, in the arena. Within the transports' untyped a page holds at
     * most one device; a platform device outside it adds one each
     * (specs/services.md). */
    uint64_t const transports_end = last_transport + 4096;
    uint32_t bus_slots = platform_count + 1;
    if (device_untyped != 0 && last_transport != 0 && transports_end > device_untyped) {
        bus_slots += static_cast<uint32_t>((transports_end - device_untyped) / 4096);
    }
    aegir::director::Device *bus = static_cast<aegir::director::Device *>(
        arena.allocate(sizeof(aegir::director::Device) * bus_slots));
    uint32_t bus_count = 0;
    if (device_untyped != 0 && last_transport != 0) {
        failures += survey_devices(bootinfo, allocator, scratch, device_untyped,
                                   first_transport, last_transport, platform, platform_count,
                                   bus, bus_slots, &bus_count);
    }

    heading("memory");
    write("  untyped: ");
    number(allocator.untyped_count());
    write(" caps, ");
    report_size(allocator.normal_bytes());
    write(" normal, ");
    report_size(allocator.device_bytes());
    write(" device\n");

    bool const memory_ok = self_test(allocator, scratch, system);
    write("  self test: ");
    write(memory_ok ? "retyped, mapped, wrote and read back a page\n"
                    : "FAILED (see above)\n");

    write("  window: ");
    aegir::debug_write_hex(scratch.base());
    write("..");
    aegir::debug_write_hex(scratch.limit());
    write(", ");
    report_size(scratch.mapped_bytes());
    write(" mapped\n");
    write("  charged to `system`: ");
    report_size(system.bytes);
    write(" in ");
    number(system.objects);
    write(" objects\n");

    aegir::spawn::Initrd initrd(static_cast<void const *>(_cpio_archive),
                                static_cast<uint64_t>(_cpio_archive_end - _cpio_archive));
    char const *manifest_text = nullptr;
    unsigned long manifest_size = 0;
    bool const initrd_ok = read_initrd(initrd, manifest_text, manifest_size);

    aegir::manifest::Manifest manifest(arena, system);
    bool manifest_ok = false;
    if (initrd_ok) {
        if (!manifest.parse(manifest_text, static_cast<uint32_t>(manifest_size))) {
            aegir::manifest::Manifest::Problem found = manifest.problem();
            write("  FAIL line ");
            number(found.line);
            write(": ");
            write(found.message);
            write("\n");
            ++failures;
        } else {
            report_manifest(manifest);
            manifest_ok = true;
        }
    }

    bool booted = false;
    if (initrd_ok && manifest_ok) {
        /* A device's frame goes into the service as it stands: the survey unmaps each one as
         * it reads, so nothing holds a mapping of it and no copy is needed. Which device a
         * service is given is its section's `device_id`, looked up in the survey's list by
         * Services::boot (specs/services.md, specs/authority.md). */
        /* The device goes to the one service that declares itself the device
         * manager (manifests/services.manifest, `device_manager`), which is the
         * thing that was missing when every spawn was handed the same frame.
         */
        /* What director delegates to the device manager beyond the kit every
         * spawner is given (apps/aegir-director/src/services.cc): interrupt
         * issue belongs to whoever turns the tree's devices into drivers.
         * IRQControl is the kernel's one well of handler caps (manual, io.tex),
         * and the device manager is that service. The cap moves rather than
         * copies -- a copy derives to a *null* cap (kernel/src/object/
         * objecttype.c:75-78) -- so custody changes hands whole, and the root
         * task's slot is empty from here. */
        static char const kIrqControlName[] = "irqcontrol";
        aegir::spawn::PortGrant const delegated[] = {
            {kIrqControlName, sizeof(kIrqControlName) - 1, 0, seL4_CapIRQControl,
             seL4_AllRights, 0, 0, true},
        };
        /* The platform devices a service claims by compatible are taken now,
         * after the manifest says which are wanted: reaching a page in the
         * middle of a device untyped costs every page before it, so only the
         * claimed devices are worth the walk (specs/services.md). */
        failures += survey_claimed_platform_devices(bootinfo, allocator, platform,
                                                    platform_count, manifest, bus, bus_slots,
                                                    &bus_count);
        booted = boot_services(initrd, manifest, allocator, scratch, arena, system, device_tree,
                               device_tree_bytes, bus, bus_count, delegated, 1);
    }

    /* Director's own inbox. Nothing signals it yet; it exists so the boot thread
     * has something to *block* on at the end, which it must do rather than spin --
     * it runs at seL4_MaxPrio, so a spinning root task starves every service below
     * it. That is not a theory: it is why the supervisor received faults it could
     * never report, and why the client's lines after its readiness signal were
     * missing from every transcript. Restarts and the elevation path arrive here
     * later, so this is the right place for the root task to sleep. */
    seL4_Error inbox_error = seL4_NoError;
    seL4_CPtr const director_inbox =
        allocator.alloc_object(seL4_NotificationObject, seL4_NotificationBits, system,
                               &inbox_error);
    if (director_inbox == 0) {
        write("  FAIL director's own notification could not be created\n");
        ++failures;
    }

    if (failures == 0 && memory_ok && initrd_ok && manifest_ok && booted) {
        write("\nAEGIR_BOOT_OK\n");
    } else {
        write("\nAEGIR_BOOT_INCOMPLETE\n");
    }

    if (director_inbox == 0) {
        /* Already reported incomplete; there is nothing to wait on. */
        aegir::halt();
    }

    /* Boot is done: sleep until something needs the root task. A root task has
     * nothing to return to. */
    for (;;) {
        seL4_Word badge = 0;
        seL4_Wait(director_inbox, &badge);
    }
}
