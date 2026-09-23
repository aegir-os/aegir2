/*
 * aegir-fs-smoke: the filesystem library's acceptance client.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A spawned boot service that speaks the VFS through both halves of the
 * library split (specs/cxx.md step 5): the hosted aegir::filesystem wrapper
 * for the enumeration std::filesystem has no path for, and the freestanding
 * aegir::vfs transport for resolving and reading. This file is the seL4-facing
 * half and must not include a libc++ header (checks.h explains why); the
 * wrapper's check is the libc++ half.
 */

#include "checks.h"

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/vfs.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

using aegir::vfs::Namespace;
using aegir::vfs::Volume;

/* Static, like the smokes': the allocator's tables and the scratch window's
 * bookkeeping are tens of kilobytes, and a service's stack is pages. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

int g_failed = 0;

void report(bool ok, char const *what)
{
    aegir::debug_write(ok ? "  fs-smoke: ok: " : "  fs-smoke: FAIL: ");
    aegir::debug_write(what);
    aegir::debug_write("\n");
    if (!ok) {
        ++g_failed;
    }
}

bool same_bytes(char const *a, char const *b, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

/* The mapping authority the spawn kit installs: the delegated untyped, the
 * VSpace root and the free-address window. The pattern is the cxx-smoke's. */
bool adopt_memory()
{
    uint64_t untyped_slot = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    uint64_t untyped_physical = 0;
    uint32_t untyped_bits = 0;
    uint64_t untyped_address = 0;
    static_cast<void>(aegir::bootstrap::untyped(&untyped_physical, &untyped_bits,
                                                &untyped_address));

    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            if (entry.kind == aegir::bootstrap::EntryKind::Capability &&
                entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            }
        }
    }

    bool ok = aegir::bootstrap::capability("untyped", 7, &untyped_slot) &&
              aegir::bootstrap::capability("vspace", 6, &vspace_slot) &&
              aegir::bootstrap::window(&window_base, &window_bytes) &&
              g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                      untyped_physical);
    if (ok) {
        g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::kCNodeBits) - first_free, 0);
        ok = g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                             static_cast<uintptr_t>(window_base),
                             static_cast<uintptr_t>(window_base + window_bytes), &g_objects);
    }
    return ok;
}

/* Resolve a path, asking again while the volume is not registered yet: the
 * volumes join the namespace while the boot set is coming up, and not yet is
 * the same answer as never (the test bed's resolve says the same). */
bool resolve_wait(Namespace &space, char const *path, uint32_t length, seL4_CPtr slot,
                  Namespace::Resolved &out) noexcept
{
    for (;;) {
        if (space.resolve(path, length, slot, out)) {
            return true;
        }
        seL4_Yield();
    }
}

/* Read up to `want` bytes from the start, asking again at the next offset
 * until the file ends -- one answer carries at most a volume's kReadMax. */
uint32_t read_prefix(Volume &volume, char const *path, uint32_t length, char *destination,
                     uint32_t want) noexcept
{
    uint64_t offset = 0;
    uint32_t have = 0;
    while (have < want) {
        Volume::Bytes bytes{};
        if (!volume.read(path, length, offset, want - have, bytes)) {
            break;
        }
        for (uint64_t i = 0; i < bytes.count && have < want; ++i) {
            destination[have++] = bytes.data[i];
        }
        offset += bytes.count;
        if (bytes.eof || bytes.count == 0) {
            break;
        }
    }
    return have;
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::debug_write("\nfs-smoke: Aegir's filesystem, through the libraries\n");

    if (!adopt_memory()) {
        aegir::debug_write("  fs-smoke: FAIL no untyped, vspace or window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    constexpr uint64_t kHeapBytes = 4ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        aegir::debug_write("  fs-smoke: FAIL the heap could not claim the window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    Namespace space = Namespace::find();
    report(space.valid(), "the vfs.namespace port was given to this process");

    /* A read from a read-only volume the boot always has. */
    {
        Namespace::Resolved resolved{};
        seL4_CPtr const slot = g_objects.alloc_slot();
        char buffer[64] = {};
        uint32_t have = 0;
        if (slot != 0 &&
            resolve_wait(space, "Initrd:services.manifest", sizeof("Initrd:services.manifest") - 1,
                         slot, resolved)) {
            Volume volume(resolved.volume);
            have = read_prefix(volume, resolved.rest, resolved.rest_length, buffer, sizeof(buffer));
        }
        report(have >= 7 && same_bytes(buffer, "# Aegir", 7),
               "Initrd:services.manifest reads and begins '# Aegir'");
    }

    /* stat: a path's kind and size without listing a directory -- what
     * std::filesystem::status stands on. The root is a directory; a file is
     * a file with a size. */
    {
        Namespace::Resolved resolved{};
        seL4_CPtr const slot = g_objects.alloc_slot();
        Volume::Info info{};
        bool const file_ok =
            slot != 0 &&
            resolve_wait(space, "Initrd:services.manifest",
                         sizeof("Initrd:services.manifest") - 1, slot, resolved) &&
            Volume(resolved.volume).stat(resolved.rest, resolved.rest_length, info) &&
            info.kind == aegir::volume::kKindFile && info.size > 0;

        Namespace::Resolved root{};
        seL4_CPtr const root_slot = g_objects.alloc_slot();
        Volume::Info root_info{};
        bool const dir_ok =
            root_slot != 0 &&
            resolve_wait(space, "Initrd:", sizeof("Initrd:") - 1, root_slot, root) &&
            Volume(root.volume).stat(root.rest, root.rest_length, root_info) &&
            root_info.kind == aegir::volume::kKindDir;
        report(file_ok && dir_ok, "stat reports a file's kind and size and a root's kind");
    }

    /* A directory listing on the initrd volume: the same filesystem the read
     * above used, so this proves list() without racing the test bed on the FAT
     * volumes it walks. (Listing a FAT volume from a second client while the
     * test bed reads it corrupts the test bed's AEGIR walk -- a filesystem bug
     * this smoke found, recorded in specs/cxx.md step 5 rather than worked
     * around by never reading a FAT volume.) */
    {
        Namespace::Resolved resolved{};
        seL4_CPtr const slot = g_objects.alloc_slot();
        bool found = false;
        if (slot != 0 &&
            resolve_wait(space, "Initrd:", sizeof("Initrd:") - 1, slot, resolved)) {
            Volume volume(resolved.volume);
            for (uint64_t index = 0;; ++index) {
                Volume::Entry entry{};
                if (!volume.list(resolved.rest, resolved.rest_length, index, entry)) {
                    break;
                }
                if (entry.name_length == sizeof("services.manifest") - 1 &&
                    same_bytes(entry.name, "services.manifest", entry.name_length)) {
                    found = true;
                }
            }
        }
        report(found, "Initrd: lists services.manifest");
    }

    /* A directory listing on a FAT volume -- the case that exposed the shared
     * window: a second client lists while the test bed reads, and with a
     * window per client neither disturbs the other (aegir/block.h). */
    {
        Namespace::Resolved resolved{};
        seL4_CPtr const slot = g_objects.alloc_slot();
        bool found = false;
        if (slot != 0 && resolve_wait(space, "AEGIR:", sizeof("AEGIR:") - 1, slot, resolved)) {
            Volume volume(resolved.volume);
            for (uint64_t index = 0;; ++index) {
                Volume::Entry entry{};
                if (!volume.list(resolved.rest, resolved.rest_length, index, entry)) {
                    break;
                }
                if (entry.name_length == 9 && same_bytes(entry.name, "AEGIR.TXT", 9)) {
                    found = true;
                }
            }
        }
        report(found, "AEGIR: lists AEGIR.TXT");
    }

    /* The hosted wrapper, in its own translation unit (the libc++ half). It
     * runs here, after the resolves above have waited for the namespace to
     * have volumes -- volumes() is a one-shot, as std::filesystem's calls
     * are. */
    g_failed += aegir::fs_smoke::run();

    /* The write side, on the writable scratch volume, under a name of our own
     * so the test bed and this smoke never collide -- and, with a window per
     * client, never corrupt each other either. */
    {
        Namespace::Resolved resolved{};
        seL4_CPtr const slot = g_objects.alloc_slot();
        char const text[] = "aegir filesystem\n";
        bool ok = false;
        if (slot != 0 &&
            resolve_wait(space, "SCRATCH:FS_SMOKE.TXT",
                         sizeof("SCRATCH:FS_SMOKE.TXT") - 1, slot, resolved)) {
            Volume volume(resolved.volume);
            uint64_t const handle =
                volume.open(resolved.rest, resolved.rest_length,
                            aegir::volume::kOpenCreate | aegir::volume::kOpenTruncate);
            bool const wrote = handle != 0 &&
                               volume.write(handle, text, sizeof(text) - 1) &&
                               volume.close(handle);
            char buffer[sizeof(text)] = {};
            uint32_t const have = wrote ? read_prefix(volume, resolved.rest,
                                                      resolved.rest_length, buffer,
                                                      sizeof(text) - 1)
                                        : 0;
            bool const matches = have == sizeof(text) - 1 &&
                                 same_bytes(buffer, text, sizeof(text) - 1);
            bool const removed = volume.remove(resolved.rest, resolved.rest_length);
            ok = wrote && matches && removed;
        }
        report(ok, "SCRATCH: writes, reads back and removes a file");
    }

    aegir::debug_write(g_failed == 0 ? "FS_SMOKE_OK\n" : "FS_SMOKE_FAIL\n");

    /* The boot thread waits for this, so the marker can follow. */
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    return g_failed;
}
