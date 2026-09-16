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

/** What the device tree says the machine has. */
class DeviceReport : public aegir::devtree::Tree::Visitor {
public:
    bool device(aegir::devtree::Device const &device) override {
        if (!device.has_region) {
            return true;
        }
        ++with_region;
        if (same_string(device.compatible, device.compatible_length, "virtio,mmio")) {
            ++virtio_mmio;
            aegir::debug_write("    ");
            aegir::debug_write_hex(device.base);
            if (device.has_interrupt) {
                aegir::debug_write(" irq ");
                aegir::debug_write_unsigned(device.interrupt);
            } else {
                aegir::debug_write(" (no interrupt)");
            }
            aegir::debug_write("\n");
        }
        return true;
    }

    unsigned with_region = 0;
    unsigned virtio_mmio = 0;

    static bool same_string(char const *text, uint32_t length, char const *wanted) noexcept {
        for (uint32_t i = 0; i < length; ++i) {
            if (text[i] != wanted[i]) {
                return false;
            }
            if (wanted[i] == '\0') {
                return true;
            }
        }
        return wanted[length] == '\0';
    }
};

/** Count the devices the tree describes, and report the buses in it.
 *
 *  The blob lives in the extra bootinfo pages, whose frame capabilities the kernel
 *  put in our CSpace, and it is read in place: the scratch window only ever hands
 *  out the next page, so mapping them in order gives one contiguous window, and
 *  nothing is unmapped while the tree points into it. Where the blob *starts* in
 *  those pages is not something we assume -- the magic says so.
 *
 *  This is all of Aegir's device discovery for now: the device manager, which owns
 *  the bus -> device -> service map, is not spawned yet (specs/services.md). */
unsigned report_devices(seL4_BootInfo const *bootinfo, aegir::mem::Scratch *scratch) noexcept {
    uint32_t const pages =
        static_cast<uint32_t>(bootinfo->extraBIPages.end - bootinfo->extraBIPages.start);
    if (pages == 0) {
        aegir::debug_write("  FAIL the kernel gave us no pages for a device tree\n");
        return 1;
    }

    uint8_t const *blob = nullptr;
    uint64_t mapped = 0;
    for (seL4_CPtr cap = bootinfo->extraBIPages.start; cap < bootinfo->extraBIPages.end; ++cap) {
        /* These frames are already mapped: the kernel puts the extra bootinfo
         * pages -- which is where the device tree lives -- into the root task's
         * address space, and a frame cannot be mapped at two addresses
         * ("RISCVPageMap: attempting to map frame into multiple addresses"). So
         * move each one into the scratch window, because the tree is read in place
         * and needs its pages to be contiguous to be walked. */
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
    aegir::debug_write(" KiB of it, ");
    aegir::debug_write_unsigned(tree.reserved_entries());
    aegir::debug_write(" reserved regions\n");

    DeviceReport report;
    if (!tree.walk(report)) {
        aegir::debug_write("  FAIL the device tree could not be read: stopped ");
        aegir::debug_write_unsigned(tree.failure_offset());
        aegir::debug_write(" of ");
        aegir::debug_write_unsigned(tree.struct_size());
        aegir::debug_write(" struct bytes, ");
        aegir::debug_write_unsigned(tree.strings_size());
        aegir::debug_write(" string bytes\n");
        return 1;
    }
    aegir::debug_write("  devices: ");
    aegir::debug_write_unsigned(report.with_region);
    aegir::debug_write(" with a register window, of which ");
    aegir::debug_write_unsigned(report.virtio_mmio);
    aegir::debug_write(" are virtio transports\n");
    return 0;
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
        write("\n");
    }
}

/* Create the boot set and wait for it to say it is ready. Supervision is the
 * spawner's other half (specs/director.md): every process gets a notification,
 * and a service that never signals is a boot that does not finish -- which is the
 * honest outcome while there is no timer to time out on. */
bool boot_services(aegir::spawn::Initrd const &initrd, aegir::manifest::Manifest const &manifest,
                   aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch,
                   aegir::mem::Arena &arena, aegir::mem::Account &account) noexcept
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
    services.boot(manifest, account, started, boot, &supervisor);

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
        write("  allocator: ");
        number(allocator.untyped_free());
        write(" untyped free, largest 2^");
        number(allocator.largest_free_bits());
        write("; last request 2^");
        number(allocator.last_request_bits());
        write(" from a 2^");
        number(allocator.last_candidate_bits());
        write(" candidate\n");
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

    aegir::mem::Allocator allocator(bootinfo);
    if (!allocator.initialise()) {
        problem("the allocator could not take the machine's memory");
    }

    aegir::mem::Scratch scratch(bootinfo);
    if (!scratch.initialise()) {
        problem("the address space window could not be worked out");
    }

    /* The device tree, before anything is spawned: it is what the device manager
     * will own, and it is the only place Aegir learns what the machine is
     * (specs/services.md). */
    failures += report_devices(bootinfo, &scratch);

    /* Everything boot allocates is charged to the system account
     * (specs/authority.md). Its capacity grows on demand; there is no ceiling
     * chosen here. */
    aegir::mem::Account system{"system", 0, 0, 0};
    aegir::mem::Arena arena(allocator, scratch, system);

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
        booted = boot_services(initrd, manifest, allocator, scratch, arena, system);
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
