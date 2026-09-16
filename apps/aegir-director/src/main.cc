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
#include <aegir/manifest.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/vspace.h>

#include <sel4/sel4.h>

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

/* The name director looks its manifest up by: the flat name the archive uses,
 * which is the file's basename. */
constexpr char const *kManifestEntry = "services.manifest";

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

/* Names in a newc archive sit in the header's name field, which is NUL
 * terminated (its length includes the NUL). The API hands back a pointer without
 * the length, so this is how the length is recovered -- bounded by the archive's
 * longest name. */
unsigned name_length(char const *name, unsigned limit) noexcept
{
    unsigned length = 0;
    while (length < limit && name[length] != '\0') {
        ++length;
    }
    return length;
}

void write_name(char const *name, unsigned length) noexcept
{
    for (unsigned i = 0; i < length; ++i) {
        seL4_DebugPutChar(name[i]);
    }
}

bool same_name(char const *left, unsigned left_length, char const *right,
               unsigned right_length) noexcept
{
    if (left_length != right_length) {
        return false;
    }
    for (unsigned i = 0; i < left_length; ++i) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
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
bool read_initrd(char const *&manifest, unsigned long &manifest_size) noexcept
{
    auto const *archive = static_cast<void const *>(_cpio_archive);
    unsigned long const length = static_cast<unsigned long>(_cpio_archive_end - _cpio_archive);

    struct cpio_info info {};
    if (cpio_info(archive, length, &info) != 0) {
        problem("the initrd could not be read");
        return false;
    }

    heading("initrd (flat: names are identities, there are no paths)");
    bool duplicate = false;
    for (unsigned int i = 0; i < info.file_count; ++i) {
        char const *name = nullptr;
        unsigned long size = 0;
        if (cpio_get_entry(archive, length, static_cast<int>(i), &name, &size) == nullptr) {
            problem("an initrd entry could not be read");
            return false;
        }
        unsigned const name_size = name_length(name, info.max_path_sz);
        for (unsigned int j = 0; j < i && !duplicate; ++j) {
            char const *other_name = nullptr;
            unsigned long other_size = 0;
            if (cpio_get_entry(archive, length, static_cast<int>(j), &other_name, &other_size) ==
                nullptr) {
                continue;
            }
            duplicate = same_name(name, name_size, other_name, name_length(other_name, info.max_path_sz));
        }
        write("  ");
        write_name(name, name_size);
        write("  ");
        number(size);
        write(" bytes\n");
    }
    if (duplicate) {
        problem("two initrd entries share a name, so one of them shadows the other");
        return false;
    }

    auto *found = static_cast<char const *>(
        cpio_get_file(archive, length, kManifestEntry, &manifest_size));
    if (found == nullptr) {
        write("  FAIL no entry named ");
        write(kManifestEntry);
        write("\n");
        ++failures;
        return false;
    }
    manifest = found;
    return true;
}

bool report_manifest(char const *text, unsigned long length, aegir::mem::Arena &arena,
                     aegir::mem::Account &account) noexcept
{
    aegir::manifest::Manifest manifest(arena, account);
    if (!manifest.parse(text, length)) {
        aegir::manifest::Manifest::Problem found = manifest.problem();
        write("  FAIL line ");
        number(found.line);
        write(": ");
        write(found.message);
        write("\n");
        ++failures;
        return false;
    }

    heading("boot manifest");
    write("  ");
    number(manifest.size());
    write(" service(s) declared\n");
    for (uint32_t i = 0; i < manifest.size(); ++i) {
        aegir::manifest::Entry const &entry = manifest[i];
        write("  ");
        write_name(entry.name.data, entry.name.length);
        write(": binary ");
        write_name(entry.binary.data, entry.binary.length);
        write(", authority ");
        write(entry.authority == aegir::manifest::Authority::System ? "system" : "user");
        write(", account ");
        write_name(entry.account.data, entry.account.length);
        write("\n");
    }
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

    char const *manifest_text = nullptr;
    unsigned long manifest_size = 0;
    bool const initrd_ok = read_initrd(manifest_text, manifest_size);

    bool manifest_ok = false;
    if (initrd_ok) {
        manifest_ok = report_manifest(manifest_text, manifest_size, arena, system);
    }

    if (failures == 0 && memory_ok && initrd_ok && manifest_ok) {
        write("\nAEGIR_BOOT_OK\n");
    } else {
        write("\nAEGIR_BOOT_INCOMPLETE\n");
    }

    /* A root task has nothing to return to. */
    aegir::halt();
}
