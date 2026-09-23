/*
 * The runtime's file calls -- implementation. See files.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * musl's filesystem functions (and so libc++'s std::filesystem) issue Linux
 * syscalls; the heap's dispatcher answers them here, in the process's own
 * memory, by talking to the VFS instead of the kernel. The pieces:
 *
 *   - a resolved path is an Aegir path -- "Volume:rest" -- or a relative one,
 *     joined to the process's current directory (environment.md). A volume
 *     capability is minted by the namespace for the call; transient calls
 *     reuse one slot, an open file keeps its own until close.
 *   - the fd table is per-process and grows on demand: an open file is a
 *     volume capability, the volume-relative path, a write handle when it is
 *     open for writing, and the read cursor. A directory is the same minus the
 *     handle, with a listing cursor.
 *   - statx, openat, read, write, lseek, getdents64, mkdirat, unlinkat,
 *     chdir, getcwd and fcntl are the calls the standard library actually
 *     makes; each answers a value or a negative errno.
 *
 * This is the seL4 side of the seL4/libc++ boundary (heap.cc's header says
 * why), so it includes aegir/vfs.h and no libc++ header. It also must not
 * include musl's <string.h>: seL4's headers declare strcpy with C++ linkage
 * and one translation unit cannot have both (specs/userland.md).
 */

#define _GNU_SOURCE 1

#include "files.h"

#include <aegir/bootstrap.h>
#include <aegir/mem/allocator.h>
#include <aegir/vfs.h>
#include <aegir/volume.h>
#include <sel4/sel4.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>

namespace aegir::heap::files {

namespace {

/* The longest path anything here holds: the protocol's own bound, not a
 * number of ours (aegir/nmspace.h). */
constexpr uint32_t kPathCapacity = aegir::nmspace::kPathMax;

/* The stat family on this architecture is musl's kstat path, not statx:
 * src/stat/fstatat.c's `SYS_fstatat` is `newfstatat`, and its 64-bit
 * `st_atime_sec` makes the statx fallback unnecessary, so `fstatat_kstat`
 * runs and the kernel's `struct kstat` is what a stat call writes
 * (arch/riscv64/kstat.h). The layout is musl's, copied so the two agree. */
struct Kstat {
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    unsigned long __pad;
    off_t st_size;
    blksize_t st_blksize;
    int __pad2;
    blkcnt_t st_blocks;
    long st_atime_sec;
    long st_atime_nsec;
    long st_mtime_sec;
    long st_mtime_nsec;
    long st_ctime_sec;
    long st_ctime_nsec;
    unsigned __unused[2];
};

/* The kernel's dirent64, which musl's readdir reads straight into its own
 * struct dirent (they agree field for field). */
struct Dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

constexpr uint8_t kDirentDirectory = 4; /* DT_DIR */
constexpr uint8_t kDirentRegular = 8;   /* DT_REG */
constexpr uint32_t kDirentHeader = 8 + 8 + 2 + 1;

aegir::mem::Allocator *g_allocator = nullptr;

/* The current directory: a plain buffer the runtime owns, so chdir, getcwd,
 * std::filesystem::current_path and aegir::environment all read one string
 * (environment.md). Loaded once from the bootstrap block the spawner wrote. */
char g_cwd[kPathCapacity];
uint32_t g_cwd_length = 0;
bool g_cwd_loaded = false;

/* The namespace, found once. A process that holds no vfs.namespace -- a
 * program that never said it needs one -- finds an invalid port and every
 * call is refused. */
aegir::vfs::Namespace &namespace_port() noexcept
{
    static aegir::vfs::Namespace space = aegir::vfs::Namespace::find();
    return space;
}

char const *current_dir(uint32_t *length) noexcept
{
    if (!g_cwd_loaded) {
        g_cwd_loaded = true;
        uint32_t found_length = 0;
        char const *found = aegir::bootstrap::current_dir(&found_length);
        if (found != nullptr && found_length > 0 && found_length <= kPathCapacity) {
            for (uint32_t i = 0; i < found_length; ++i) {
                g_cwd[i] = found[i];
            }
            g_cwd_length = found_length;
        }
    }
    if (length != nullptr) {
        *length = g_cwd_length;
    }
    return g_cwd_length != 0 ? g_cwd : nullptr;
}

void set_current_dir(char const *path, uint32_t length) noexcept
{
    for (uint32_t i = 0; i < length; ++i) {
        g_cwd[i] = path[i];
    }
    g_cwd_length = length;
    g_cwd_loaded = true;
}

uint32_t text_length(char const *text) noexcept
{
    uint32_t length = 0;
    while (text != nullptr && text[length] != '\0') {
        ++length;
    }
    return length;
}

void copy_text(char *destination, char const *source, uint32_t length) noexcept
{
    for (uint32_t i = 0; i < length; ++i) {
        destination[i] = source[i];
    }
}

/* A path is absolute when it names a volume before any slash: Aegir's shape is
 * "Volume:rest", and everything after the colon (and the aliases) is the VFS's
 * to substitute. A path with no colon is relative and joins the current
 * directory. */
bool names_a_volume(char const *path, uint32_t length) noexcept
{
    for (uint32_t i = 0; i < length; ++i) {
        if (path[i] == ':') {
            return true;
        }
        if (path[i] == '/') {
            return false;
        }
    }
    return false;
}

bool absolute_path(char const *path, uint32_t length, char *out, uint32_t capacity,
                   uint32_t *out_length) noexcept
{
    if (names_a_volume(path, length)) {
        if (length > capacity) {
            return false;
        }
        copy_text(out, path, length);
        *out_length = length;
        return true;
    }
    uint32_t cwd_length = 0;
    char const *cwd = current_dir(&cwd_length);
    if (cwd == nullptr) {
        return false;
    }
    uint32_t const total = cwd_length + (length != 0 ? 1 + length : 0);
    if (total > capacity) {
        return false;
    }
    copy_text(out, cwd, cwd_length);
    if (length != 0) {
        out[cwd_length] = '/';
        copy_text(out + cwd_length + 1, path, length);
    }
    *out_length = total;
    return true;
}

/* One resolve's result: the capability (in the slot the caller named) and the
 * volume-relative path, copied out because the namespace's own rest buffer is
 * only valid until its next call. */
struct Target {
    seL4_CPtr volume;
    char rest[kPathCapacity];
    uint32_t rest_length;
    char volume_name[nmspace::kNameMax]; /* the name the caller wrote, before ':' */
    uint32_t volume_name_length;
};

/* Resolve `path` (relative or absolute) into `slot`. False when the path is
 * relative and there is no current directory, the volume is unknown, or the
 * capability did not arrive. */
bool resolve_target(char const *path, uint32_t length, seL4_CPtr slot,
                    Target &out) noexcept
{
    char full[kPathCapacity];
    uint32_t full_length = 0;
    if (!absolute_path(path, length, full, kPathCapacity, &full_length)) {
        return false;
    }
    aegir::vfs::Namespace &space = namespace_port();
    if (!space.valid()) {
        return false;
    }
    aegir::vfs::Namespace::Resolved resolved{};
    if (!space.resolve(full, full_length, slot, resolved) ||
        resolved.rest_length > kPathCapacity) {
        return false;
    }
    copy_text(out.rest, resolved.rest, resolved.rest_length);
    out.rest_length = resolved.rest_length;
    out.volume = slot;
    /* The volume part of the absolute path, as written: two resolves of the
     * same volume through different aliases answer with different aliases, so
     * only the spelling can say whether two paths share one volume -- and
     * rename must not cross volumes. */
    uint32_t name_length = 0;
    while (name_length < full_length && full[name_length] != ':') {
        ++name_length;
    }
    if (name_length > sizeof(out.volume_name)) {
        name_length = sizeof(out.volume_name);
    }
    copy_text(out.volume_name, full, name_length);
    out.volume_name_length = name_length;
    return true;
}

/* ---- capability slots ---- */

/* A slot pool, so open/close and every stat do not exhaust the CSpace: the
 * allocator hands a slot out once and never takes it back, but a slot whose
 * capability has been deleted is free again, and this keeps a list of them. */
seL4_CPtr *g_free_slots = nullptr;
uint32_t g_free_count = 0;
uint32_t g_free_capacity = 0;

seL4_CPtr take_slot() noexcept
{
    if (g_free_count != 0) {
        return g_free_slots[--g_free_count];
    }
    return g_allocator != nullptr ? g_allocator->alloc_slot() : 0;
}

void empty_slot(seL4_CPtr slot) noexcept
{
    if (slot != 0) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, slot,
                          aegir::bootstrap::kCNodeBits);
    }
}

void give_slot(seL4_CPtr slot) noexcept
{
    if (slot == 0) {
        return;
    }
    empty_slot(slot);
    if (g_free_count == g_free_capacity) {
        uint32_t const capacity = g_free_capacity != 0 ? g_free_capacity * 2 : 8;
        auto *grown = static_cast<seL4_CPtr *>(
            realloc(g_free_slots, capacity * sizeof(seL4_CPtr)));
        if (grown == nullptr) {
            return; /* the slot is empty and lost; the CSpace is the failure */
        }
        g_free_slots = grown;
        g_free_capacity = capacity;
    }
    g_free_slots[g_free_count++] = slot;
}

/* One slot for the transient resolves (stat, mkdir, remove): the capability is
 * deleted after each so the next resolve can mint into it again. */
seL4_CPtr transient_slot() noexcept
{
    static seL4_CPtr slot = take_slot();
    return slot;
}

/* ---- the fd table ---- */

struct Entry {
    bool used;
    bool directory;
    bool readable;
    bool writable;
    seL4_CPtr volume;
    uint64_t handle; /* the write-side handle, or 0 */
    char path[kPathCapacity];
    uint32_t path_length;
    uint64_t offset; /* read/lseek cursor */
    uint64_t index;  /* directory listing cursor */
};

Entry *g_entries = nullptr;
uint32_t g_entry_capacity = 0;

constexpr int kNoFd = -1;

Entry *entry_for(int fd) noexcept
{
    if (fd < 3 || static_cast<uint32_t>(fd) >= g_entry_capacity) {
        return nullptr;
    }
    return g_entries[fd].used ? &g_entries[fd] : nullptr;
}

int alloc_fd() noexcept
{
    for (uint32_t i = 3;; ++i) {
        if (i >= g_entry_capacity) {
            uint32_t capacity = g_entry_capacity != 0 ? g_entry_capacity : 16;
            while (capacity <= i) {
                capacity *= 2;
            }
            auto *grown =
                static_cast<Entry *>(realloc(g_entries, capacity * sizeof(Entry)));
            if (grown == nullptr) {
                return kNoFd;
            }
            for (uint32_t j = g_entry_capacity; j < capacity; ++j) {
                grown[j].used = false;
            }
            g_entries = grown;
            g_entry_capacity = capacity;
        }
        if (!g_entries[i].used) {
            g_entries[i].used = true;
            return static_cast<int>(i);
        }
    }
}

int install(seL4_CPtr slot, Target const &target, uint64_t handle, bool directory,
            bool readable, bool writable) noexcept
{
    int const fd = alloc_fd();
    if (fd == kNoFd) {
        if (handle != 0) {
            aegir::vfs::Volume(slot).close(handle);
        }
        give_slot(slot);
        return -EMFILE;
    }
    Entry &entry = g_entries[fd];
    entry.directory = directory;
    entry.readable = readable;
    entry.writable = writable;
    entry.volume = slot;
    entry.handle = handle;
    entry.offset = 0;
    entry.index = 0;
    entry.path_length = target.rest_length;
    copy_text(entry.path, target.rest, target.rest_length);
    return fd;
}

void fill_kstat(Kstat *out, uint64_t kind, uint64_t size) noexcept
{
    *out = Kstat{};
    out->st_mode = static_cast<mode_t>(
        kind == aegir::volume::kKindDir ? S_IFDIR | 0755 : S_IFREG | 0644);
    out->st_nlink = 1;
    out->st_ino = 1;
    out->st_size = size;
    out->st_blksize = 4096;
    out->st_blocks = (size + 511) / 512;
}

/* One file's kind and size into a kstat, by path or by the fd that names it. */
bool stat_target(char const *path, uint32_t length, Kstat *out) noexcept
{
    seL4_CPtr const slot = transient_slot();
    if (slot == 0) {
        return false;
    }
    Target target{};
    bool ok = false;
    if (resolve_target(path, length, slot, target)) {
        aegir::vfs::Volume::Info info{};
        ok = aegir::vfs::Volume(target.volume)
                 .stat(target.rest, target.rest_length, info);
        if (ok) {
            fill_kstat(out, info.kind, info.size);
        }
    }
    empty_slot(slot);
    return ok;
}

bool stat_entry(Entry const &entry, Kstat *out) noexcept
{
    aegir::vfs::Volume::Info info{};
    if (!aegir::vfs::Volume(entry.volume).stat(entry.path, entry.path_length, info)) {
        return false;
    }
    fill_kstat(out, info.kind, info.size);
    return true;
}

}  // namespace

void adopt(aegir::mem::Allocator &allocator) noexcept
{
    g_allocator = &allocator;
}

long newfstatat(int dfd, char const *path, void *buffer, int flags) noexcept
{
    static_cast<void>(flags);
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    if (path == nullptr || buffer == nullptr) {
        return -EFAULT;
    }
    if (path[0] == '\0' && dfd >= 0) {
        return fstat(dfd, buffer);
    }
    if (dfd != AT_FDCWD) {
        return -ENOENT;
    }
    if (!stat_target(path, text_length(path), static_cast<Kstat *>(buffer))) {
        return -ENOENT;
    }
    return 0;
}

long fstat(int fd, void *buffer) noexcept
{
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    if (buffer == nullptr) {
        return -EFAULT;
    }
    Entry *entry = entry_for(fd);
    if (entry == nullptr) {
        return -EBADF;
    }
    if (!stat_entry(*entry, static_cast<Kstat *>(buffer))) {
        return -ENOENT;
    }
    return 0;
}

long openat(int dfd, char const *path, int flags, int mode) noexcept
{
    static_cast<void>(mode);
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    if (path == nullptr) {
        return -EFAULT;
    }
    if (dfd != AT_FDCWD) {
        /* A directory fd as the anchor is not answered yet: the calls libc++'s
         * filesystem makes in the common paths pass AT_FDCWD. */
        return -ENOENT;
    }
    seL4_CPtr const slot = take_slot();
    if (slot == 0) {
        return -EMFILE;
    }
    Target target{};
    if (!resolve_target(path, text_length(path), slot, target)) {
        give_slot(slot);
        return -ENOENT;
    }
    aegir::vfs::Volume volume(target.volume);
    aegir::vfs::Volume::Info info{};
    bool const exists = volume.stat(target.rest, target.rest_length, info);

    if ((flags & O_DIRECTORY) != 0) {
        if (!exists || info.kind != aegir::volume::kKindDir) {
            give_slot(slot);
            return exists ? -ENOTDIR : -ENOENT;
        }
        return install(slot, target, 0, true, false, false);
    }
    if ((flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0) {
        /* The volume's open takes its create flag as the intent to name a file
         * for writing; an open without O_CREAT still only opens an existing
         * one, so the missing case is refused here rather than letting the
         * filesystem make a file the caller did not ask for. */
        if (!exists && (flags & O_CREAT) == 0) {
            give_slot(slot);
            return -ENOENT;
        }
        uint64_t volume_flags = aegir::volume::kOpenCreate;
        if ((flags & O_TRUNC) != 0) {
            volume_flags |= aegir::volume::kOpenTruncate;
        }
        uint64_t const handle = volume.open(target.rest, target.rest_length, volume_flags);
        if (handle == 0) {
            give_slot(slot);
            return exists ? -EACCES : -ENOENT;
        }
        bool const readable = (flags & O_RDWR) != 0;
        return install(slot, target, handle, false, readable, true);
    }
    if (!exists) {
        give_slot(slot);
        return -ENOENT;
    }
    if (info.kind == aegir::volume::kKindDir) {
        give_slot(slot);
        return -EISDIR;
    }
    return install(slot, target, 0, false, true, false);
}

long close(int fd) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr) {
        return -EBADF;
    }
    if (entry->writable && entry->handle != 0) {
        aegir::vfs::Volume(entry->volume).close(entry->handle);
    }
    give_slot(entry->volume);
    entry->used = false;
    entry->volume = 0;
    entry->handle = 0;
    entry->path_length = 0;
    return 0;
}

long read(int fd, void *buffer, size_t count) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr || !entry->readable || entry->directory || buffer == nullptr) {
        return -EBADF;
    }
    auto *bytes_out = static_cast<char *>(buffer);
    size_t total = 0;
    while (total < count) {
        /* `bytes.data` points into the Volume, so it must outlive the copy. */
        aegir::vfs::Volume volume(entry->volume);
        aegir::vfs::Volume::Bytes bytes{};
        if (!volume.read(entry->path, entry->path_length, entry->offset, count - total,
                         bytes) ||
            bytes.count == 0) {
            break;
        }
        for (uint64_t i = 0; i < bytes.count; ++i) {
            bytes_out[total + i] = bytes.data[i];
        }
        total += bytes.count;
        entry->offset += bytes.count;
        if (bytes.eof) {
            break;
        }
    }
    return static_cast<long>(total);
}

long write(int fd, void const *buffer, size_t count) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr || !entry->writable || entry->handle == 0 ||
        buffer == nullptr) {
        return -EBADF;
    }
    auto const *bytes_in = static_cast<char const *>(buffer);
    size_t total = 0;
    while (total < count) {
        uint32_t const chunk = static_cast<uint32_t>(
            count - total < aegir::volume::kWriteMax ? count - total
                                                     : aegir::volume::kWriteMax);
        uint64_t written = 0;
        if (!aegir::vfs::Volume(entry->volume)
                 .write(entry->handle, bytes_in + total, chunk, &written) ||
            written == 0) {
            break;
        }
        total += written;
    }
    return static_cast<long>(total);
}

long lseek(int fd, long offset, int whence) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr || entry->directory) {
        return -EBADF;
    }
    int64_t base = 0;
    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        base = static_cast<int64_t>(entry->offset);
    } else if (whence == SEEK_END) {
        aegir::vfs::Volume::Info info{};
        if (!aegir::vfs::Volume(entry->volume)
                 .stat(entry->path, entry->path_length, info)) {
            return -EIO;
        }
        base = static_cast<int64_t>(info.size);
    } else {
        return -EINVAL;
    }
    int64_t const position = base + offset;
    if (position < 0) {
        return -EINVAL;
    }
    entry->offset = static_cast<uint64_t>(position);
    return static_cast<long>(position);
}

long getdents(int fd, void *buffer, size_t count) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr || !entry->directory || buffer == nullptr) {
        return -EBADF;
    }
    auto *out = static_cast<uint8_t *>(buffer);
    size_t used = 0;
    for (;;) {
        /* `listed.name` points into the Volume, so it must outlive the copy. */
        aegir::vfs::Volume volume(entry->volume);
        aegir::vfs::Volume::Entry listed{};
        if (!volume.list(entry->path, entry->path_length, entry->index, listed)) {
            break; /* end of directory (or a refusal, which reads as one) */
        }
        uint32_t const name_length = listed.name_length;
        uint32_t const record = (kDirentHeader + name_length + 1 + 7) & ~7u;
        if (used + record > count) {
            break; /* the caller's buffer is full; the next call continues */
        }
        auto *dirent = reinterpret_cast<Dirent64 *>(out + used);
        dirent->d_ino = entry->index + 1;
        dirent->d_off = static_cast<int64_t>(entry->index + 1);
        dirent->d_reclen = static_cast<uint16_t>(record);
        dirent->d_type = listed.kind == aegir::volume::kKindDir ? kDirentDirectory
                                                               : kDirentRegular;
        copy_text(dirent->d_name, listed.name, name_length);
        dirent->d_name[name_length] = '\0';
        used += record;
        entry->index += 1;
    }
    return static_cast<long>(used);
}

long mkdirat(int dfd, char const *path, int mode) noexcept
{
    static_cast<void>(mode);
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    if (path == nullptr) {
        return -EFAULT;
    }
    if (dfd != AT_FDCWD) {
        return -ENOENT;
    }
    seL4_CPtr const slot = transient_slot();
    if (slot == 0) {
        return -EMFILE;
    }
    long result = -EACCES;
    Target target{};
    if (resolve_target(path, text_length(path), slot, target)) {
        aegir::vfs::Volume volume(target.volume);
        aegir::vfs::Volume::Info info{};
        if (volume.stat(target.rest, target.rest_length, info)) {
            /* It is already something: a fresh directory is EEXIST, and
             * create_directory reads that as "fine if it is a directory". */
            result = -EEXIST;
        } else if (volume.make_directory(target.rest, target.rest_length)) {
            result = 0;
        }
    }
    empty_slot(slot);
    return result;
}

long unlinkat(int dfd, char const *path, int flags) noexcept
{
    static_cast<void>(flags);
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    if (path == nullptr) {
        return -EFAULT;
    }
    if (dfd != AT_FDCWD) {
        return -ENOENT;
    }
    seL4_CPtr const slot = transient_slot();
    if (slot == 0) {
        return -EMFILE;
    }
    long result = -ENOENT;
    Target target{};
    if (resolve_target(path, text_length(path), slot, target) &&
        aegir::vfs::Volume(target.volume)
            .remove(target.rest, target.rest_length)) {
        result = 0;
    }
    empty_slot(slot);
    return result;
}

/* Rename must not cross volumes -- the filesystem would interpret the other
 * volume's path in its own tree. Two resolves of one volume answer with
 * different capabilities, so the caller's own spelling of the volume name is
 * what can be compared; an alias and the name it points at are not caught
 * (specs/fat.md). */
bool same_volume(Target const &a, Target const &b) noexcept
{
    if (a.volume_name_length != b.volume_name_length) {
        return false;
    }
    for (uint32_t i = 0; i < a.volume_name_length; ++i) {
        char ca = a.volume_name[i];
        char cb = b.volume_name[i];
        if (ca >= 'a' && ca <= 'z') {
            ca = static_cast<char>(ca - ('a' - 'A'));
        }
        if (cb >= 'a' && cb <= 'z') {
            cb = static_cast<char>(cb - ('a' - 'A'));
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

long renameat2(int old_dfd, char const *old_path, int new_dfd, char const *new_path,
               unsigned flags) noexcept
{
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    if (old_path == nullptr || new_path == nullptr) {
        return -EFAULT;
    }
    if (old_dfd != AT_FDCWD || new_dfd != AT_FDCWD) {
        return -ENOENT;
    }
    if (flags != 0) {
        return -EINVAL; /* no NOREPLACE or EXCHANGE yet */
    }
    /* Two resolves need two capabilities, so this takes two from the pool and
     * gives them both back -- the transient single slot cannot carry both. */
    seL4_CPtr const old_slot = take_slot();
    seL4_CPtr const new_slot = take_slot();
    if (old_slot == 0 || new_slot == 0) {
        give_slot(old_slot);
        give_slot(new_slot);
        return -EMFILE;
    }
    long result = -ENOENT;
    Target old_target{};
    Target new_target{};
    if (resolve_target(old_path, text_length(old_path), old_slot, old_target) &&
        resolve_target(new_path, text_length(new_path), new_slot, new_target)) {
        if (!same_volume(old_target, new_target)) {
            result = -EXDEV;
        } else if (aegir::vfs::Volume(old_target.volume)
                       .rename(old_target.rest, old_target.rest_length,
                               new_target.rest, new_target.rest_length)) {
            result = 0;
        }
    }
    give_slot(old_slot);
    give_slot(new_slot);
    return result;
}

long truncate(char const *path, long length) noexcept
{
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    if (path == nullptr) {
        return -EFAULT;
    }
    if (length < 0) {
        return -EINVAL;
    }
    seL4_CPtr const slot = transient_slot();
    if (slot == 0) {
        return -EMFILE;
    }
    long result = -ENOENT;
    Target target{};
    if (resolve_target(path, text_length(path), slot, target) &&
        aegir::vfs::Volume(target.volume)
            .truncate(target.rest, target.rest_length, static_cast<uint64_t>(length))) {
        result = 0;
    }
    empty_slot(slot);
    return result;
}

long ftruncate(int fd, long length) noexcept
{
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    if (length < 0) {
        return -EINVAL;
    }
    Entry *entry = entry_for(fd);
    if (entry == nullptr) {
        return -EBADF;
    }
    if (!entry->writable) {
        return -EINVAL; /* an fd not open for writing cannot be resized */
    }
    if (!aegir::vfs::Volume(entry->volume)
             .truncate(entry->path, entry->path_length,
                       static_cast<uint64_t>(length))) {
        return -ENOENT;
    }
    /* The fd's own read cursor must not point past the new end. */
    if (entry->offset > static_cast<uint64_t>(length)) {
        entry->offset = static_cast<uint64_t>(length);
    }
    return 0;
}

long chdir(char const *path) noexcept
{
    if (path == nullptr) {
        return -EFAULT;
    }
    char full[kPathCapacity];
    uint32_t length = 0;
    if (!absolute_path(path, text_length(path), full, kPathCapacity, &length)) {
        return -ENOENT;
    }
    set_current_dir(full, length);
    return 0;
}

long getcwd(char *buffer, size_t size) noexcept
{
    if (buffer == nullptr) {
        return -EFAULT;
    }
    if (g_cwd_length == 0) {
        static_cast<void>(current_dir(nullptr));
    }
    if (g_cwd_length == 0) {
        return -ENOENT;
    }
    if (size < static_cast<size_t>(g_cwd_length) + 1) {
        return -ERANGE;
    }
    copy_text(buffer, g_cwd, g_cwd_length);
    buffer[g_cwd_length] = '\0';
    return static_cast<long>(g_cwd_length) + 1;
}

long fcntl(int fd, int command, long argument) noexcept
{
    static_cast<void>(argument);
    if (entry_for(fd) == nullptr) {
        return -EBADF;
    }
    /* No descriptor flags are kept: close-on-exec has nothing to close over
     * (there is no exec), and the open-mode question is answered from the
     * entry's own flags, which is all musl's opendir asks. */
    if (command == F_GETFD || command == F_SETFD || command == F_GETFL) {
        return 0;
    }
    return -EINVAL;
}

}  // namespace aegir::heap::files

/* The current directory's two ends, for aegir::environment -- which is the
 * hosted C++ face of the same state chdir/getcwd answer. Plain C so the
 * libc++-facing translation unit can declare them (specs/environment.md). */
extern "C" char const *aegir_heap_current_dir(uint32_t *length) noexcept
{
    return aegir::heap::files::current_dir(length);
}

extern "C" int aegir_heap_set_current_dir(char const *path, uint32_t length) noexcept
{
    if (path == nullptr && length != 0) {
        return -1;
    }
    if (length > aegir::heap::files::kPathCapacity) {
        return -1;
    }
    aegir::heap::files::set_current_dir(path, length);
    return 0;
}
