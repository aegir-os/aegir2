/*
 * aegir-fs-fat: the FAT filesystem service.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One instance per FAT partition, started by the partition manager with the
 * block device's port and a range grant (offset and length) rather than the
 * whole device (specs/services.md). FAT16/32/ExFAT are the interchange
 * filesystems -- how Aegir exchanges data with the rest of the world, not its
 * own filesystem. FAT32 writes when the descriptor row says the volume is
 * writable: open/write/close, the handle side of the volume protocol
 * (specs/vfs.md).
 *
 * It proves the attachment out loud (parse the BPB, list the root directory,
 * read the file the disk was made with back), announces the volume's label
 * on the partition manager's own port -- the manager registers it with the
 * VFS, because a service not in the manifest is given its ports rather than
 * declaring them (specs/vfs.md) -- and then serves the volume protocol on
 * "vol": component paths under the colon, directories walked and made.
 */

#include "fat.h"

#include <aegir/block.h>
#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/descriptor.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/nmspace.h>
#include <aegir/partman.h>
#include <aegir/volume.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

constexpr uint32_t kSectorBytes = 512;

uint32_t word32(uint8_t const *at) noexcept
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(at[i]) << (i * 8);
    }
    return value;
}

void put32(uint8_t *at, uint32_t value) noexcept
{
    for (uint32_t i = 0; i < 4; ++i) {
        at[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

/* What the serve phase works through: the block port, the shared window the
 * reads land in, the volume's own geometry, and the range grant's offset.
 * Static because a spawned process's stack is two pages (specs/userland.md)
 * and the answer buffers below are not small. */
aegir::ipc::Consumer g_blk;
uint8_t *g_window = nullptr;
uint64_t g_first = 0;
uint32_t g_window_sectors = 0;
uint32_t g_cluster_count = 0;
aegir::fat::Volume g_volume{};

/* The write side's state: whether this volume takes writes at all (the
 * descriptor row's word, the partition manager's statement), and the handle
 * table -- the one page of memory the spawner gave us, zeroed below, where
 * each row is one open file. The table's bound is the grant, and reaching
 * it is a loud refusal, never a quiet overwrite (specs/vfs.md). */
bool g_writable = false;
uint8_t *g_memory = nullptr;
uint32_t g_memory_bytes = 0;
uint64_t g_handle_serial = 0;

/* One open file. The serial is the handle the client names; it is never
 * reused, so a stale handle answers "not one" rather than naming a new
 * file. The badge is whose it is: resolve minted the client's port copy
 * with it, and a handle named by any other badge is not one. The dirent
 * locator is where the file's own slot lives on disk, so a write can patch
 * its size and first cluster. */
struct Handle {
    uint64_t serial;
    uint64_t badge;
    uint64_t cursor;
    uint64_t size;
    uint64_t dirent_sector; /* volume-relative */
    uint32_t dirent_index;  /* which 32-byte slot of that sector */
    uint32_t first_cluster;
};

Handle *handle_lookup(uint64_t serial, uint64_t badge) noexcept
{
    if (serial == 0 || g_memory == nullptr) {
        return nullptr;
    }
    uint32_t const capacity = g_memory_bytes / static_cast<uint32_t>(sizeof(Handle));
    auto *rows = reinterpret_cast<Handle *>(g_memory);
    for (uint32_t i = 0; i < capacity; ++i) {
        if (rows[i].serial == serial) {
            return rows[i].badge == badge ? rows + i : nullptr;
        }
    }
    return nullptr;
}

Handle *handle_alloc(uint64_t badge) noexcept
{
    if (g_memory == nullptr) {
        return nullptr;
    }
    uint32_t const capacity = g_memory_bytes / static_cast<uint32_t>(sizeof(Handle));
    auto *rows = reinterpret_cast<Handle *>(g_memory);
    for (uint32_t i = 0; i < capacity; ++i) {
        if (rows[i].serial == 0) {
            rows[i].serial = ++g_handle_serial;
            rows[i].badge = badge;
            return rows + i;
        }
    }
    return nullptr;
}

/* One read, volume-relative: the data lands in the window, which every parse
 * then reads. False is a report, not a hang. A read larger than the window
 * refuses rather than chunking, and that is a bound the FAT format itself
 * already keeps: every structure walked here is one sector or one cluster,
 * and the specification caps a cluster at 64 KiB -- exactly this window's
 * size. */
bool read(uint64_t lba, uint32_t count) noexcept
{
    if (count > g_window_sectors) {
        return false;
    }
    aegir::ipc::Reply const reply =
        g_blk.call(aegir::block::kMethodRead, aegir::block::pack_read(g_first + lba, count));
    return reply.error == 0 && reply.word == count;
}

/* The write's half of the same call: the window's content goes to the disk.
 * The packing is the read's own (aegir/block.h -- the word is
 * direction-agnostic). */
bool write_back(uint64_t lba, uint32_t count) noexcept
{
    if (count > g_window_sectors) {
        return false;
    }
    aegir::ipc::Reply const reply =
        g_blk.call(aegir::block::kMethodWrite, aegir::block::pack_read(g_first + lba, count));
    return reply.error == 0 && reply.word == count;
}

/* The FAT sector holding one cluster's entry, read into the window; the
 * answer is its offset from the FAT's start. FAT32 only: the write side is
 * FAT32's (FAT16 refuses before it gets here). */
bool fat_load(uint32_t cluster, uint64_t *sector_out) noexcept
{
    uint64_t const rel = (static_cast<uint64_t>(cluster) * 4) / kSectorBytes;
    if (!read(g_volume.fat_start + rel, 1)) {
        return false;
    }
    *sector_out = rel;
    return true;
}

/* Write one cluster's FAT entry -- in every FAT copy, because a volume whose
 * copies disagree is a corrupt volume. */
bool fat_store(uint32_t cluster, uint32_t value) noexcept
{
    uint64_t rel = 0;
    if (!fat_load(cluster, &rel)) {
        return false;
    }
    aegir::fat::set_next32(g_window, cluster % (kSectorBytes / 4), value);
    for (uint32_t f = 0; f < g_volume.fats; ++f) {
        if (!write_back(g_volume.fat_start + f * g_volume.fat_sectors + rel, 1)) {
            return false;
        }
    }
    return true;
}

/* A fresh cluster joins a chain zeroed: a multiuser system does not leak
 * one file's old sectors into another. */
bool zero_cluster(uint32_t cluster) noexcept
{
    uint32_t const bytes = g_volume.sectors_per_cluster * kSectorBytes;
    for (uint32_t i = 0; i < bytes; ++i) {
        g_window[i] = 0;
    }
    return write_back(aegir::fat::cluster_sector(g_volume, cluster),
                      g_volume.sectors_per_cluster);
}

/* The first free cluster the FAT knows, claimed and zeroed. Zero is the
 * answer for a full volume as much as for a broken one -- the caller refuses
 * the write, and the count says how much landed. The scan is linear and
 * holds its FAT sector across the entries it covers: 128 clusters per read. */
uint32_t alloc_cluster() noexcept
{
    uint64_t held = ~0ull; /* the FAT sector in the window, when one is */
    for (uint32_t c = 2; c < 2 + g_cluster_count; ++c) {
        uint64_t const rel = (static_cast<uint64_t>(c) * 4) / kSectorBytes;
        if (rel != held) {
            if (!read(g_volume.fat_start + rel, 1)) {
                return 0;
            }
            held = rel;
        }
        if (aegir::fat::next32(g_window, c % (kSectorBytes / 4)) != aegir::fat::kFreeCluster) {
            continue;
        }
        if (!fat_store(c, aegir::fat::kEocMark32) || !zero_cluster(c)) {
            return 0;
        }
        return c;
    }
    return 0;
}

/* Free a whole chain (truncate's half): every entry back to free, in every
 * FAT copy. */
bool chain_free(uint32_t first) noexcept
{
    uint32_t c = first;
    while (c >= 2 && c < aegir::fat::kEoc32) {
        uint64_t rel = 0;
        if (!fat_load(c, &rel)) {
            return false;
        }
        uint32_t const next = aegir::fat::next32(g_window, c % (kSectorBytes / 4));
        if (!fat_store(c, aegir::fat::kFreeCluster)) {
            return false;
        }
        c = next;
    }
    return true;
}

/* The cluster holding the handle's cursor, allocating and linking when the
 * write is growing the file. Zero: the volume is full or the chain broke. */
uint32_t chain_seek(Handle *handle) noexcept
{
    uint32_t const cluster_bytes = g_volume.sectors_per_cluster * kSectorBytes;
    uint32_t const target = static_cast<uint32_t>(handle->cursor / cluster_bytes);
    uint32_t c = handle->first_cluster;
    if (c < 2) {
        c = alloc_cluster();
        if (c == 0) {
            return 0;
        }
        handle->first_cluster = c;
    }
    for (uint32_t i = 0; i < target; ++i) {
        uint64_t rel = 0;
        if (!fat_load(c, &rel)) {
            return 0;
        }
        uint32_t next = aegir::fat::next32(g_window, c % (kSectorBytes / 4));
        if (next >= aegir::fat::kEoc32) {
            next = alloc_cluster();
            if (next == 0 || !fat_store(c, next)) {
                return 0;
            }
        }
        c = next;
    }
    return c;
}

char upper(char c) noexcept
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c;
}

/* FAT names are case-insensitive; a client that asks for "aegir.txt" means
 * "AEGIR.TXT". */
bool same_name(char const *a, uint32_t a_length, char const *b, uint32_t b_length) noexcept
{
    if (a_length != b_length) {
        return false;
    }
    for (uint32_t i = 0; i < a_length; ++i) {
        if (upper(a[i]) != upper(b[i])) {
            return false;
        }
    }
    return true;
}

/* One directory, as the walks name one: the root -- a fixed region on
 * FAT16, a cluster chain like any other on FAT32, so the flag matters only
 * there -- or a subdirectory, always a chain. */
struct Dir {
    uint32_t cluster; /* first cluster of the chain; unused when root */
    bool root;        /* the FAT16 fixed root region */
};

Dir dir_root() noexcept
{
    return g_volume.fat32 ? Dir{g_volume.root_cluster, false} : Dir{0, true};
}

/* Walk one directory for one entry: by name when `name` is given, else the
 * index-th entry the directory holds. False is not-found, which a protocol
 * answer reports by saying nothing. */
bool find_in_dir(Dir dir, char const *name, uint32_t name_length, uint32_t index,
                 aegir::fat::Dirent *out) noexcept
{
    uint32_t seen = 0;
    bool found = false;
    auto consider = [&](uint32_t entry_count) -> bool {
        for (uint32_t i = 0; i < entry_count; ++i) {
            aegir::fat::Dirent dirent;
            aegir::fat::Entry const kind = aegir::fat::dirent(g_window + i * 32, &dirent);
            if (kind == aegir::fat::Entry::End) {
                return true;
            }
            if (kind == aegir::fat::Entry::Skip) {
                continue;
            }
            if (name != nullptr) {
                if (same_name(dirent.name, dirent.name_length, name, name_length)) {
                    *out = dirent;
                    found = true;
                    return true;
                }
            } else if (seen++ == index) {
                *out = dirent;
                found = true;
                return true;
            }
        }
        return false;
    };
    if (dir.root) {
        for (uint32_t s = 0; s < g_volume.root_sectors && !found; ++s) {
            if (!read(g_volume.root_start + s, 1)) {
                return false;
            }
            if (consider(kSectorBytes / 32)) {
                break;
            }
        }
        return found;
    }
    /* A chain like any other; the step is the one place the flavors differ. */
    uint32_t const eoc = g_volume.fat32 ? aegir::fat::kEoc32 : aegir::fat::kEoc16;
    uint32_t cluster = dir.cluster;
    while (!found && cluster >= 2 && cluster < eoc) {
        if (!read(aegir::fat::cluster_sector(g_volume, cluster),
                  g_volume.sectors_per_cluster)) {
            return false;
        }
        if (consider(g_volume.sectors_per_cluster * kSectorBytes / 32)) {
            break;
        }
        uint32_t const fat_offset = cluster * (g_volume.fat32 ? 4u : 2u);
        if (!read(g_volume.fat_start + fat_offset / kSectorBytes, 1)) {
            return false;
        }
        cluster = g_volume.fat32
                      ? aegir::fat::next32(g_window, cluster % (kSectorBytes / 4))
                      : aegir::fat::next16(g_window, cluster % (kSectorBytes / 2));
    }
    return found;
}

/* The components of a path, walked from the root. Each nonempty component
 * names a directory to descend into; an empty one is the parent -- the
 * Amiga convention, and the parent of the root is the root. With
 * `through_last` the whole path is directories (list); without it the walk
 * stops before the last component, which the caller's method interprets
 * (read, open). False: a component was not there, or was no directory, or
 * the path had no last component for a method that needs one. The stack is
 * the walk's own history, and its bound is the path bound's: a component is
 * at least a name and a slash. */
bool walk(char const *path, uint32_t path_length, bool through_last, Dir *dir,
          char const **last, uint32_t *last_length) noexcept
{
    static Dir stack[aegir::nmspace::kPathMax / 2 + 1];
    uint32_t depth = 0;
    *dir = dir_root();
    *last = nullptr;
    *last_length = 0;
    uint32_t at = 0;
    while (at <= path_length) {
        uint32_t end = at;
        while (end < path_length && path[end] != '/') {
            ++end;
        }
        bool const final = end == path_length;
        if (final && !through_last) {
            if (end == at) {
                return false; /* a trailing slash names no file */
            }
            *last = path + at;
            *last_length = end - at;
            return true;
        }
        if (end == at) {
            /* The parent: pop, staying at the root. */
            if (depth > 0) {
                *dir = stack[--depth];
            }
        } else {
            aegir::fat::Dirent dirent;
            if (!find_in_dir(*dir, path + at, end - at, 0, &dirent) ||
                !dirent.directory) {
                return false;
            }
            stack[depth++] = *dir;
            *dir = Dir{dirent.first_cluster, false};
        }
        if (final) {
            return true;
        }
        at = end + 1;
    }
    return true;
}

/* What a walk of a directory for a name came back with. */
enum class Slot : uint32_t {
    Found,  /* the name is there; the locator is its slot */
    Free,   /* the name is not; the locator is a slot a new entry may take */
    Full,   /* no free slot and the chain would not grow: the volume is full */
    Broken, /* a read or a write underneath failed */
};

/* Walk one directory's chain (FAT32: every directory is a chain, the root
 * included, and a full one grows like any file's), reporting where `name`
 * lives when it does and the first slot a new entry could take when it does
 * not -- an End or deleted slot, or, the chain being full, a fresh cluster
 * linked on and zeroed. */
Slot dir_slot(Dir dir, char const *name, uint32_t name_length, aegir::fat::Dirent *out,
              uint64_t *sector_out, uint32_t *index_out) noexcept
{
    bool have_free = false;
    uint64_t free_sector = 0;
    uint32_t free_index = 0;
    uint32_t cluster = dir.cluster;
    uint32_t last = cluster;
    bool ended = false;
    while (!ended && cluster >= 2 && cluster < aegir::fat::kEoc32) {
        last = cluster;
        uint64_t const at = aegir::fat::cluster_sector(g_volume, cluster);
        for (uint32_t s = 0; s < g_volume.sectors_per_cluster && !ended; ++s) {
            if (!read(at + s, 1)) {
                return Slot::Broken;
            }
            for (uint32_t i = 0; i < kSectorBytes / 32; ++i) {
                uint8_t const *raw = g_window + i * 32;
                if (raw[0] == 0x00) {
                    /* End: nothing past here is used, so this slot is free
                     * whether or not a deleted one was seen first. */
                    if (!have_free) {
                        free_sector = at + s;
                        free_index = i;
                        have_free = true;
                    }
                    ended = true;
                    break;
                }
                if (raw[0] == 0xe5) {
                    if (!have_free) {
                        free_sector = at + s;
                        free_index = i;
                        have_free = true;
                    }
                    continue;
                }
                aegir::fat::Dirent dirent;
                if (aegir::fat::dirent(raw, &dirent) == aegir::fat::Entry::Used &&
                    name != nullptr &&
                    same_name(dirent.name, dirent.name_length, name, name_length)) {
                    *out = dirent;
                    *sector_out = at + s;
                    *index_out = i;
                    return Slot::Found;
                }
            }
        }
        if (ended) {
            break;
        }
        uint64_t rel = 0;
        if (!fat_load(cluster, &rel)) {
            return Slot::Broken;
        }
        cluster = aegir::fat::next32(g_window, cluster % (kSectorBytes / 4));
    }
    if (have_free) {
        *sector_out = free_sector;
        *index_out = free_index;
        return Slot::Free;
    }
    /* No End anywhere: the chain is full, and the root grows like any chain. */
    uint32_t const fresh = alloc_cluster();
    if (fresh == 0) {
        return Slot::Full;
    }
    if (!fat_store(last, fresh)) {
        return Slot::Broken;
    }
    *sector_out = aegir::fat::cluster_sector(g_volume, fresh);
    *index_out = 0;
    return Slot::Free;
}

void answer_open(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                 uint64_t badge) noexcept
{
    uint64_t handle = 0; /* the refusal */
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                       &path_length)) {
        port.reply_words(&handle, 1);
        return;
    }
    uint32_t const path_words = 1 + (path_length + 7) / 8;
    /* The walk ends at the directory the file lives in; the last component
     * is the file. And the write side is FAT32's: FAT16 refuses. */
    Dir dir;
    char const *last = nullptr;
    uint32_t last_length = 0;
    uint8_t name83[11];
    if (count < path_words + 1 || !g_writable || !g_volume.fat32 ||
        !walk(path, path_length, false, &dir, &last, &last_length) ||
        !aegir::fat::name_83(last, last_length, name83)) {
        port.reply_words(&handle, 1);
        return;
    }
    uint64_t const flags = words[path_words];

    aegir::fat::Dirent dirent;
    uint64_t dirent_sector = 0;
    uint32_t dirent_index = 0;
    uint32_t first_cluster = 0;
    uint64_t size = 0;
    Slot const slot = dir_slot(dir, last, last_length, &dirent, &dirent_sector,
                               &dirent_index);
    bool ok = false;
    if (slot == Slot::Found && !dirent.directory) {
        /* An existing name without `create` is refused: opening for write is
         * meaning to remake the file, and the flag is the meaning. Truncate
         * frees the old chain at once; without it the file keeps its chain
         * and the cursor overwrites from the start. */
        if ((flags & aegir::volume::kOpenCreate) != 0) {
            if ((flags & aegir::volume::kOpenTruncate) != 0) {
                ok = chain_free(dirent.first_cluster);
                if (ok && read(dirent_sector, 1)) {
                    aegir::fat::dirent_update(g_window + dirent_index * 32, 0, 0);
                    ok = write_back(dirent_sector, 1);
                }
            } else {
                first_cluster = dirent.first_cluster;
                size = dirent.bytes;
                ok = true;
            }
        }
    } else if (slot == Slot::Free) {
        /* The locator is the free slot: the new entry is written there. */
        if ((flags & aegir::volume::kOpenCreate) != 0 && read(dirent_sector, 1)) {
            aegir::fat::dirent_make(g_window + dirent_index * 32, name83);
            ok = write_back(dirent_sector, 1);
        }
    }
    if (ok) {
        Handle *row = handle_alloc(badge);
        if (row != nullptr) {
            row->cursor = 0;
            row->size = size;
            row->dirent_sector = dirent_sector;
            row->dirent_index = dirent_index;
            row->first_cluster = first_cluster;
            handle = row->serial;
        }
    }
    port.reply_words(&handle, 1);
}

void answer_write(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                  uint64_t badge) noexcept
{
    uint64_t written = 0;
    if (count < 2) {
        port.reply_words(&written, 1);
        return;
    }
    Handle *handle = handle_lookup(words[0], badge);
    uint64_t const bytes = words[1];
    if (handle == nullptr || bytes > aegir::volume::kWriteMax ||
        count < 2 + (bytes + 7) / 8) {
        port.reply_words(&written, 1);
        return;
    }
    auto const *data = reinterpret_cast<uint8_t const *>(words + 2);
    uint32_t const cluster_bytes = g_volume.sectors_per_cluster * kSectorBytes;
    while (written < bytes) {
        uint32_t const cluster = chain_seek(handle);
        if (cluster == 0) {
            break;
        }
        uint64_t const in_cluster = handle->cursor % cluster_bytes;
        uint64_t const lba = aegir::fat::cluster_sector(g_volume, cluster) +
                             in_cluster / kSectorBytes;
        uint32_t const at = static_cast<uint32_t>(in_cluster % kSectorBytes);
        uint64_t const left = bytes - written;
        uint32_t const take =
            left < kSectorBytes - at ? static_cast<uint32_t>(left) : kSectorBytes - at;
        /* A sector the write covers whole needs no read-modify-write; any
         * other shape keeps what it does not touch. */
        if ((at != 0 || take != kSectorBytes) && !read(lba, 1)) {
            break;
        }
        for (uint32_t i = 0; i < take; ++i) {
            g_window[at + i] = data[written + i];
        }
        if (!write_back(lba, 1)) {
            break;
        }
        written += take;
        handle->cursor += take;
    }
    /* The dirent learns the size and the first cluster on every write: a
     * crash then never shows a size the chain does not back. */
    if (handle->cursor > handle->size) {
        handle->size = handle->cursor;
    }
    if (written > 0 && read(handle->dirent_sector, 1)) {
        aegir::fat::dirent_update(g_window + handle->dirent_index * 32,
                                  handle->first_cluster,
                                  static_cast<uint32_t>(handle->size));
        static_cast<void>(write_back(handle->dirent_sector, 1));
    }
    port.reply_words(&written, 1);
}

void answer_close(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                  uint64_t badge) noexcept
{
    uint64_t closed = 0;
    if (count >= 1) {
        Handle *handle = handle_lookup(words[0], badge);
        if (handle != nullptr) {
            handle->serial = 0;
            closed = 1;
        }
    }
    port.reply_words(&closed, 1);
}

/* mkdir's engine: every component of the path, found or made. A component
 * that exists must be a directory; one that does not is made -- one fresh
 * cluster holding "." and ".." (the parent by its cluster, the root as 0,
 * the format's convention), then the slot in its parent. */
bool make_dirs(char const *path, uint32_t path_length) noexcept
{
    static Dir stack[aegir::nmspace::kPathMax / 2 + 1];
    uint32_t depth = 0;
    Dir dir = dir_root();
    uint32_t at = 0;
    while (at <= path_length) {
        uint32_t end = at;
        while (end < path_length && path[end] != '/') {
            ++end;
        }
        if (end == at) {
            /* The parent, like the walk's: pop, staying at the root. */
            if (depth > 0) {
                dir = stack[--depth];
            }
        } else {
            uint8_t name83[11];
            if (!aegir::fat::name_83(path + at, end - at, name83)) {
                return false;
            }
            aegir::fat::Dirent dirent;
            uint64_t sector = 0;
            uint32_t index = 0;
            Slot const slot = dir_slot(dir, path + at, end - at, &dirent, &sector, &index);
            uint32_t cluster = 0;
            if (slot == Slot::Found) {
                if (!dirent.directory) {
                    return false; /* a file is not a way through */
                }
                cluster = dirent.first_cluster;
            } else if (slot == Slot::Free) {
                cluster = alloc_cluster();
                if (cluster == 0) {
                    return false;
                }
                uint64_t const own = aegir::fat::cluster_sector(g_volume, cluster);
                if (!read(own, g_volume.sectors_per_cluster)) {
                    return false;
                }
                uint8_t dot[11];
                uint8_t dotdot[11];
                for (uint32_t i = 0; i < 11; ++i) {
                    dot[i] = ' ';
                    dotdot[i] = ' ';
                }
                dot[0] = '.';
                dotdot[0] = '.';
                dotdot[1] = '.';
                aegir::fat::dirent_make_dir(g_window, dot, cluster);
                aegir::fat::dirent_make_dir(g_window + 32, dotdot,
                                            dir.root ? 0 : dir.cluster);
                if (!write_back(own, g_volume.sectors_per_cluster) ||
                    !read(sector, 1)) {
                    return false;
                }
                aegir::fat::dirent_make_dir(g_window + index * 32, name83, cluster);
                if (!write_back(sector, 1)) {
                    return false;
                }
            } else {
                return false; /* full, or broken underneath */
            }
            stack[depth++] = dir;
            dir = Dir{cluster, false};
        }
        if (end == path_length) {
            return true;
        }
        at = end + 1;
    }
    return true;
}

void answer_mkdir(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    uint64_t made = 0;
    char const *path = nullptr;
    uint32_t path_length = 0;
    /* The root only by courtesy: an empty path is the root, which exists. */
    if (count != 0 &&
        aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                      &path_length)) {
        if (path_length == 0) {
            made = 1;
        } else if (g_writable && g_volume.fat32 && make_dirs(path, path_length)) {
            made = 1;
        }
    }
    port.reply_words(&made, 1);
}

void answer_read(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                       &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint32_t const path_words = 1 + (path_length + 7) / 8;
    if (count < path_words + 2) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const offset = words[path_words];
    uint64_t wanted = words[path_words + 1];
    /* The walk ends at the directory the file lives in; the last component
     * is the file. */
    Dir dir;
    char const *last = nullptr;
    uint32_t last_length = 0;
    aegir::fat::Dirent dirent;
    if (!walk(path, path_length, false, &dir, &last, &last_length) ||
        !find_in_dir(dir, last, last_length, 0, &dirent) || dirent.directory ||
        offset > dirent.bytes) {
        port.reply_words(nullptr, 0);
        return;
    }
    if (wanted > aegir::volume::kReadMax) {
        wanted = aegir::volume::kReadMax;
    }
    uint64_t const available = dirent.bytes - offset;
    uint64_t const got = wanted < available ? wanted : available;
    uint64_t answer[aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8];
    char *bytes = reinterpret_cast<char *>(answer + aegir::volume::kReadHeaderWords);
    /* Follow the chain, copying the overlapping part of each cluster out
     * before the next read -- the FAT step's included -- clobbers the
     * window: the "content belongs to the most recent call" caveat never
     * reaches the volume's clients (aegir/volume.h). */
    uint32_t const cluster_bytes = g_volume.sectors_per_cluster * kSectorBytes;
    uint32_t const eoc = g_volume.fat32 ? aegir::fat::kEoc32 : aegir::fat::kEoc16;
    uint32_t cluster = dirent.first_cluster;
    uint64_t position = 0;
    uint64_t filled = 0;
    while (filled < got && cluster >= 2 && cluster < eoc) {
        if (position + cluster_bytes > offset) {
            if (!read(aegir::fat::cluster_sector(g_volume, cluster),
                      g_volume.sectors_per_cluster)) {
                break;
            }
            uint64_t const at = offset > position ? offset - position : 0;
            uint64_t const room = cluster_bytes - at;
            uint64_t const take = got - filled < room ? got - filled : room;
            for (uint64_t i = 0; i < take; ++i) {
                bytes[filled + i] = g_window[at + i];
            }
            filled += take;
        }
        position += cluster_bytes;
        uint32_t const fat_offset = cluster * (g_volume.fat32 ? 4u : 2u);
        if (!read(g_volume.fat_start + fat_offset / kSectorBytes, 1)) {
            break;
        }
        cluster = g_volume.fat32
                      ? aegir::fat::next32(g_window, cluster % (kSectorBytes / 4))
                      : aegir::fat::next16(g_window, cluster % (kSectorBytes / 2));
    }
    answer[0] = filled;
    answer[1] = offset + filled >= dirent.bytes ? 1 : 0;
    port.reply_words(answer, aegir::volume::kReadHeaderWords +
                                 static_cast<uint32_t>((filled + 7) / 8));
}

void answer_list(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                       &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    /* The whole path names the directory to list; the empty path is the
     * root. The index follows the path's words. */
    uint32_t const path_words = 1 + (path_length + 7) / 8;
    Dir dir;
    char const *last = nullptr;
    uint32_t last_length = 0;
    if (count < path_words + 1 ||
        !walk(path, path_length, true, &dir, &last, &last_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    aegir::fat::Dirent dirent;
    if (!find_in_dir(dir, nullptr, 0, static_cast<uint32_t>(words[path_words]),
                     &dirent)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t answer[aegir::ipc::kMaxWords];
    uint32_t const name_words = aegir::nmspace::pack_string(
        answer, dirent.name, dirent.name_length, aegir::ipc::kMaxWords * 8 -
                                                     aegir::volume::kListTailWords * 8);
    if (name_words == 0 ||
        name_words + aegir::volume::kListTailWords > aegir::ipc::kMaxWords) {
        port.reply_words(nullptr, 0);
        return;
    }
    answer[name_words] = dirent.bytes;
    answer[name_words + 1] = dirent.directory ? aegir::volume::kKindDir
                                              : aegir::volume::kKindFile;
    port.reply_words(answer, name_words + aegir::volume::kListTailWords);
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    uint32_t instance_length = 0;
    char const *instance = aegir::bootstrap::name(&instance_length);

    aegir::ipc::Consumer const blk = aegir::ipc::Consumer::find("blk", 3);
    uint64_t window_address = 0;
    uint32_t window_bytes = 0;
    uint64_t window_physical = 0;
    if (!blk.valid() ||
        !aegir::bootstrap::shared_window(&window_address, &window_bytes,
                                         &window_physical)) {
        aegir::debug_write("      FAIL fs.fat: no block port or no shared window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The range grant, as the descriptor row the partition manager wrote:
     * where this volume starts on the device and how far it runs. Every read
     * below is volume-relative; the offset is added here and nowhere else. */
    uint64_t blob_address = 0;
    uint32_t blob_bytes = 0;
    uint64_t first = 0;
    uint64_t range_sectors = 0;
    if (!aegir::bootstrap::devices(&blob_address, &blob_bytes) || blob_bytes == 0) {
        aegir::debug_write("      FAIL fs.fat: no range grant\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    aegir::descriptor::Reader rows(reinterpret_cast<void const *>(blob_address),
                                   blob_bytes);
    char const *descriptor_name = nullptr;
    uint32_t descriptor_name_length = 0;
    while (rows.next_row()) {
        aegir::descriptor::Field field;
        while (rows.next_field(field)) {
            bool ok = false;
            if (aegir::descriptor::key_is(field, "first")) {
                first = aegir::descriptor::number(field, &ok);
            } else if (aegir::descriptor::key_is(field, "sectors")) {
                range_sectors = aegir::descriptor::number(field, &ok);
            } else if (aegir::descriptor::key_is(field, "name")) {
                descriptor_name = field.value;
                descriptor_name_length = field.value_length;
            } else if (aegir::descriptor::key_is(field, "writable")) {
                g_writable = aegir::descriptor::number(field, &ok) != 0;
            }
        }
    }
    if (first == 0 || range_sectors == 0) {
        aegir::debug_write("      FAIL fs.fat: the range grant is empty\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    auto *window = reinterpret_cast<uint8_t *>(window_address);
    uint32_t const window_sectors = window_bytes / kSectorBytes;
    /* The serve phase's half of the attachment, set once here. */
    g_blk = blk;
    g_window = window;
    g_first = first;
    g_window_sectors = window_sectors;

    if (instance != nullptr) {
        aegir::debug_write("      ");
        aegir::debug_write(instance, instance_length);
        aegir::debug_write(": sectors ");
        aegir::debug_write_unsigned(first);
        aegir::debug_write("..");
        aegir::debug_write_unsigned(first + range_sectors - 1);
        aegir::debug_write(" of the device\n");
    }

    if (!read(0, 1)) {
        aegir::debug_write("      FAIL fs.fat: the BPB would not read\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    aegir::fat::Volume volume;
    if (!aegir::fat::bpb(window, &volume)) {
        aegir::debug_write("      FAIL fs.fat: sector 0 is not a FAT BPB\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    g_volume = volume;
    g_cluster_count = static_cast<uint32_t>((range_sectors - volume.data_start) /
                                            volume.sectors_per_cluster);

    /* The handle table's page, when the spawner gave one: the block's
     * untyped entry is where a memory grant's address and size travel
     * (the same entry a driver's queue memory arrives by). Zeroed before
     * it is trusted -- a retyped frame holds whatever the last owner left,
     * and a nonzero serial would be a handle nobody opened. */
    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    if (aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address) &&
        memory_bits != 0 && memory_address != 0) {
        g_memory = reinterpret_cast<uint8_t *>(memory_address);
        g_memory_bytes = 1u << memory_bits;
        for (uint32_t i = 0; i < g_memory_bytes; ++i) {
            g_memory[i] = 0;
        }
    }

    /* The FSInfo sector's free count may legally be "unknown", and keeping
     * it would be a second copy of a truth the FAT itself carries -- mark
     * it so, once, rather than maintain it (Microsoft's FAT spec:
     * 0xffffffff says not-known). */
    if (g_writable && volume.fat32 && read(1, 1) &&
        word32(g_window) == 0x41615252u && word32(g_window + 484) == 0x61417272u) {
        put32(g_window + 488, 0xffffffffu);
        put32(g_window + 492, 0xffffffffu);
        static_cast<void>(write_back(1, 1));
    }

    aegir::debug_write("      ");
    aegir::debug_write(instance, instance_length);
    aegir::debug_write(": FAT");
    aegir::debug_write(volume.fat32 ? "32" : "16");
    aegir::debug_write(", ");
    aegir::debug_write_unsigned(volume.sectors_per_cluster);
    aegir::debug_write(" sectors per cluster, data starts at sector ");
    aegir::debug_write_unsigned(volume.data_start);
    if (g_writable) {
        aegir::debug_write(volume.fat32 ? ", writable" : ", writable but FAT16: read-only");
    }
    aegir::debug_write("\n");

    /* The root directory: a fixed region on FAT16, a cluster chain like any
     * other on FAT32. The first plain file with content is remembered, to be
     * read back below. */
    aegir::fat::Dirent target{};
    bool have_target = false;
    auto list_entries = [&](uint32_t entry_count) {
        for (uint32_t i = 0; i < entry_count; ++i) {
            aegir::fat::Dirent dirent;
            aegir::fat::Entry const kind = aegir::fat::dirent(window + i * 32, &dirent);
            if (kind == aegir::fat::Entry::End) {
                return true;
            }
            if (kind == aegir::fat::Entry::Skip || dirent.directory) {
                continue;
            }
            aegir::debug_write("      ");
            aegir::debug_write(instance, instance_length);
            aegir::debug_write(": ");
            aegir::debug_write(dirent.name, dirent.name_length);
            aegir::debug_write(", ");
            aegir::debug_write_unsigned(dirent.bytes);
            aegir::debug_write(" bytes\n");
            if (!have_target && dirent.bytes > 0) {
                target = dirent;
                have_target = true;
            }
        }
        return false;
    };

    bool walked = true;
    if (volume.fat32) {
        uint32_t cluster = volume.root_cluster;
        while (cluster < aegir::fat::kEoc32) {
            if (!read(aegir::fat::cluster_sector(volume, cluster),
                      volume.sectors_per_cluster)) {
                walked = false;
                break;
            }
            if (list_entries(volume.sectors_per_cluster * kSectorBytes / 32)) {
                break;
            }
            uint32_t const fat_offset = cluster * 4;
            if (!read(volume.fat_start + fat_offset / kSectorBytes, 1)) {
                walked = false;
                break;
            }
            cluster = aegir::fat::next32(window, cluster % (kSectorBytes / 4));
        }
    } else {
        for (uint32_t s = 0; s < volume.root_sectors && walked; ++s) {
            if (!read(volume.root_start + s, 1)) {
                walked = false;
                break;
            }
            if (list_entries(kSectorBytes / 32)) {
                break;
            }
        }
    }
    if (!walked) {
        aegir::debug_write("      FAIL fs.fat: the root directory would not read\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* Read the file back: follow its chain and print what it says. The file
     * the disk was made with is the checksum -- a reader that walked the
     * wrong sectors does not print this. */
    if (have_target) {
        uint32_t cluster = target.first_cluster;
        uint32_t left = target.bytes;
        aegir::debug_write("      ");
        aegir::debug_write(instance, instance_length);
        aegir::debug_write(": ");
        aegir::debug_write(target.name, target.name_length);
        aegir::debug_write(" says: ");
        /* The chain step is the one place the flavors differ: 4-byte entries
         * on FAT32, 2-byte on FAT16, and each with its own end-of-chain
         * floor. */
        uint32_t const eoc = volume.fat32 ? aegir::fat::kEoc32 : aegir::fat::kEoc16;
        while (left > 0 && cluster >= 2 && cluster < eoc) {
            if (!read(aegir::fat::cluster_sector(volume, cluster),
                      volume.sectors_per_cluster)) {
                aegir::debug_write("(a cluster would not read)");
                left = 0;
                break;
            }
            uint32_t const here = volume.sectors_per_cluster * kSectorBytes;
            uint32_t const shown = left < here ? left : here;
            aegir::debug_write(reinterpret_cast<char const *>(window), shown);
            left -= shown;
            uint32_t const fat_offset = cluster * (volume.fat32 ? 4u : 2u);
            if (!read(volume.fat_start + fat_offset / kSectorBytes, 1)) {
                break;
            }
            cluster = volume.fat32
                          ? aegir::fat::next32(window, cluster % (kSectorBytes / 4))
                          : aegir::fat::next16(window, cluster % (kSectorBytes / 2));
        }
        aegir::debug_write("\n");
    }

    /* The volume's public name: the label in its own boot sector when it
     * has one -- 11 space-padded bytes where the flavor keeps it -- the
     * descriptor's name otherwise, and the instance's own name as the last
     * word. The boot sector is re-read because the demo's walks have
     * clobbered the window since. */
    char label[11];
    uint32_t label_length = 0;
    if (read(0, 1)) {
        uint32_t const at = volume.fat32 ? 71 : 43;
        for (uint32_t i = 0; i < sizeof(label); ++i) {
            label[i] = window[at + i];
        }
        label_length = sizeof(label);
        while (label_length > 0 &&
               (label[label_length - 1] == ' ' || label[label_length - 1] == '\0')) {
            --label_length;
        }
    }
    char const *volume_name = label;
    uint32_t volume_name_length = label_length;
    if (volume_name_length == 0) {
        volume_name = descriptor_name;
        volume_name_length = descriptor_name_length;
    }
    if (volume_name_length == 0 && instance_length > 4) {
        volume_name = instance + 4; /* the name without the kind */
        volume_name_length = instance_length - 4;
    }

    /* Announce, then serve: the partition manager is waiting for the one
     * call, and its answer is the name the volume actually got from the
     * VFS. An empty answer is a refusal -- the volume serves anyway,
     * under no name. */
    aegir::ipc::Owner vol = aegir::ipc::Owner::find("vol", 3);
    aegir::ipc::Consumer const partman = aegir::ipc::Consumer::find(
        aegir::partman::kPortName, aegir::partman::kPortNameLength);
    if (!vol.valid()) {
        aegir::debug_write("      FAIL fs.fat: no volume port\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    if (partman.valid() && volume_name_length != 0) {
        uint64_t out[aegir::nmspace::kNameMax / 8 + 1];
        uint32_t const out_words = aegir::nmspace::pack_string(
            out, volume_name, volume_name_length, aegir::nmspace::kNameMax);
        uint64_t in[aegir::nmspace::kNameMax / 8 + 1];
        aegir::ipc::WordsReply const announced =
            partman.call_words(aegir::partman::kMethodAnnounce, out, out_words, in,
                               aegir::nmspace::kNameMax / 8 + 1);
        char const *assigned = nullptr;
        uint32_t assigned_length = 0;
        aegir::debug_write("      ");
        aegir::debug_write(instance, instance_length);
        aegir::debug_write(": ");
        if (announced.error != 0 || announced.count == 0 ||
            !aegir::nmspace::unpack_string(in, announced.count, aegir::nmspace::kNameMax,
                                           &assigned, &assigned_length)) {
            aegir::debug_write("the announce was refused\n");
        } else {
            aegir::debug_write(assigned, assigned_length);
            aegir::debug_write(": announced, serving\n");
        }
    }

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        /* The badge is whose call this is: a handle is only its owner's
         * (specs/vfs.md). */
        seL4_Word badge = 0;
        uint32_t const method =
            vol.receive_words(words, aegir::ipc::kMaxWords, &count, &badge);
        switch (method) {
        case aegir::volume::kMethodRead:
            answer_read(vol, words, count);
            break;
        case aegir::volume::kMethodList:
            answer_list(vol, words, count);
            break;
        case aegir::volume::kMethodOpen:
            answer_open(vol, words, count, badge);
            break;
        case aegir::volume::kMethodWrite:
            answer_write(vol, words, count, badge);
            break;
        case aegir::volume::kMethodClose:
            answer_close(vol, words, count, badge);
            break;
        case aegir::volume::kMethodMkdir:
            answer_mkdir(vol, words, count);
            break;
        default:
            /* A method this version does not know is answered by saying
             * nothing (specs/services.md's versioning rule). */
            vol.reply_words(nullptr, 0);
            break;
        }
    }
}
