/*
 * aegir-posix: the file surface -- implementation. See
 * include/aegir/posix/files.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * musl's filesystem functions (and so libc++'s std::filesystem) issue Linux
 * syscalls; aegir-heap's dispatcher answers them here, in the process's own
 * memory, by talking to the VFS instead of the kernel. The pieces:
 *
 *   - a path goes through the view first (specs/posix.md): `/Name/rest` is the
 *     native `Name:rest`, `/` is the synthetic root, `.` and `..` are
 *     normalized, and a path that already names a volume passes through
 *     untouched. The translation itself is the pure function in
 *     aegir/posix/path.h; posix_path is what hands it the current directory,
 *     and names_the_root is what the calls that can speak about `/` ask.
 *   - the current directory is one VFS path the layer keeps -- a native
 *     program and aegir::environment read the same string (environment.md) --
 *     with `/` as a state of its own, because no VFS path names it. chdir
 *     writes both, getcwd presents them back as the view's path.
 *   - the fd table is per-process and grows on demand: an open file is a
 *     volume capability, the volume-relative path, a write handle when it is
 *     open for writing, and the read cursor. A directory is the same minus the
 *     handle, with a listing cursor.
 *   - the synthetic root is a directory of a third kind: no volume, no path,
 *     and a listing that is the namespace's own -- every volume and every
 *     binding, one entry each -- which is what stat, openat with O_DIRECTORY
 *     and getdents answer it from.
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

#include <aegir/posix/files.h>
#include <aegir/posix/path.h>

#include <aegir/bootstrap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/metadata.h>
#include <aegir/vfs.h>
#include <aegir/volume.h>
#include <sel4/sel4.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>

namespace aegir::posix::files {

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
aegir::mem::Scratch *g_scratch = nullptr;

/* The frame a bulk write (write-frame) passes: one 4 KiB page of the runtime's
 * own, mapped so the bytes go in, with its capability kept. Claimed lazily and
 * kept for the process's life -- the heap's own mappings use map_at at the top
 * of the window, so a page at the cursor never collides with them. */
seL4_CPtr g_write_frame = 0;
uint8_t *g_write_frame_map = nullptr;

bool ensure_write_frame() noexcept
{
    if (g_write_frame != 0) {
        return true;
    }
    if (g_allocator == nullptr || g_scratch == nullptr) {
        return false;
    }
    aegir::mem::Account account{"io", 0, 0, 0};
    seL4_Error error = seL4_NoError;
    seL4_CPtr const frame = g_allocator->alloc_page(account, &error);
    if (frame == 0) {
        return false;
    }
    void *const mapped = g_scratch->map(frame);
    if (mapped == nullptr) {
        return false;
    }
    g_write_frame = frame;
    g_write_frame_map = static_cast<uint8_t *>(mapped);
    return true;
}

/* The current directory: a plain buffer the runtime owns, so chdir, getcwd,
 * std::filesystem::current_path and aegir::environment all read one string
 * (environment.md). Loaded once from the bootstrap block the spawner wrote. */
char g_cwd[kPathCapacity];
uint32_t g_cwd_length = 0;
bool g_cwd_loaded = false;

/* Whether it is the POSIX root instead (specs/posix.md): `/` is a place in the
 * view and not one in the VFS, so no VFS path names it and the state lives here
 * beside the path. `chdir("/")` is what sets it, and any VFS path clears it --
 * a VFS path is never the root -- which is why the clear sits in
 * set_current_dir, the one door a POSIX chdir's translation and a native chdir
 * both come through. */
bool g_cwd_is_root = false;

/* And which grammar `getcwd` answers it in: the view's when the directory was
 * set from a POSIX path, Aegir's when it was set natively or inherited from the
 * spawner. The *stored* string is a VFS path either way, and it has to be:
 * libc++'s std::filesystem::absolute and LLVM's sys::fs::make_absolute both
 * read it and compose with it, and both expect a `Volume:` root -- a POSIX
 * getcwd must not take that away from the process that also hosts the compiler
 * (specs/environment.md's one string, specs/cxx.md's step 5, and the fs smoke's
 * own Aegir-grammar check). */
bool g_cwd_is_posix = false;

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

void set_current_dir(char const *path, uint32_t length, bool posix_form) noexcept
{
    g_cwd_is_root = false;
    g_cwd_is_posix = posix_form;
    for (uint32_t i = 0; i < length; ++i) {
        g_cwd[i] = path[i];
    }
    g_cwd_length = length;
    g_cwd_loaded = true;
}

/* The root, which has no VFS path: the buffer holds none, and `getcwd` answers
 * `/` while this stands (specs/posix.md). */
void set_root_dir() noexcept
{
    g_cwd_is_root = true;
    g_cwd_length = 0;
    g_cwd_loaded = true;
}

/* The directory this process's own binary came from (specs/environment.md):
 * read once from the bootstrap block, which is read-only, and kept here. Not
 * the current directory: it is the process's own, never inherited, so it does
 * not change. */
char const *program_dir(uint32_t *length) noexcept
{
    static bool loaded = false;
    static char dir[kPathCapacity];
    static uint32_t dir_length = 0;
    if (!loaded) {
        loaded = true;
        uint32_t found_length = 0;
        char const *found = aegir::bootstrap::program_dir(&found_length);
        if (found != nullptr && found_length > 0 && found_length <= kPathCapacity) {
            for (uint32_t i = 0; i < found_length; ++i) {
                dir[i] = found[i];
            }
            dir_length = found_length;
        }
    }
    if (length != nullptr) {
        *length = dir_length;
    }
    return dir_length != 0 ? dir : nullptr;
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

/* ---- the POSIX view's translation (specs/posix.md) ------------------------ */

/* The path view's grammar, named apart from the many `path` parameters below. */
namespace view = aegir::posix::path;

/* The layer's current directory in the view's own form: `/Sys/Tests`, or `/`
 * for the root. Empty -- nullptr, zero -- when the process has none, which is
 * what makes a relative path refused rather than guessed at
 * (specs/environment.md).
 *
 * The layer keeps the current directory as a *VFS* path, because that is what a
 * native program and aegir::environment read (specs/environment.md: the
 * current directory is a VFS path, not a lock); the view's form of it is
 * derived here, per call, so the one string stays the one string. */
char const *posix_cwd(char *out, uint32_t *length) noexcept
{
    if (g_cwd_is_root) {
        out[0] = '/';
        *length = 1;
        return out;
    }
    uint32_t vfs_length = 0;
    char const *const vfs = current_dir(&vfs_length);
    if (vfs == nullptr || !view::present(vfs, vfs_length, out, kPathCapacity, *length)) {
        *length = 0;
        return nullptr;
    }
    return out;
}

/* The current directory as `getcwd` answers it: the view's own form when a POSIX
 * chdir set it, the Aegir grammar when a native one did or the process inherited
 * it from its spawner (specs/posix.md). The string the layer keeps is a VFS path
 * in both cases -- that is what `std::filesystem` and the compiler compose with
 * -- so a native answer is that string as it stands, and a POSIX answer is its
 * view form, which is where the mapping pays for itself. */
char const *presented_cwd(char *out, uint32_t *length) noexcept
{
    if (g_cwd_is_root) {
        out[0] = '/';
        *length = 1;
        return out;
    }
    uint32_t vfs_length = 0;
    char const *const vfs = current_dir(&vfs_length);
    if (vfs == nullptr) {
        *length = 0;
        return nullptr;
    }
    if (!g_cwd_is_posix) {
        *length = vfs_length;
        return vfs;
    }
    if (!view::present(vfs, vfs_length, out, kPathCapacity, *length)) {
        *length = 0;
        return nullptr;
    }
    return out;
}

/* The layer's one translation (specs/posix.md's boundary): a path that begins
 * with `/` is POSIX and is rewritten to the native `Volume:rest`, anything else
 * is native and passes through, and `.`/`..` are normalized because the Amiga's
 * directories have none. It is the pure function in aegir/posix/path.h; this is
 * what hands it the current directory, and `where` says whether a VFS path came
 * back or the synthetic root did. */
bool posix_path(char const *given, uint32_t length, char *out,
                view::Translation &where) noexcept
{
    char cwd[kPathCapacity];
    uint32_t cwd_length = 0;
    char const *const base = posix_cwd(cwd, &cwd_length);
    return view::translate(given, length, base, cwd_length, out, kPathCapacity, where);
}

/* Whether a path names the synthetic root: `/`, `/.`, `/..`, `..` from below
 * it. Only the calls that can say something about the root ask -- stat, openat
 * with O_DIRECTORY, chdir, mkdirat -- because every other call wants a volume
 * to talk to, which the root is not (specs/posix.md). */
bool names_the_root(char const *given, uint32_t length) noexcept
{
    char full[kPathCapacity];
    view::Translation where{};
    return posix_path(given, length, full, where) && where.target == view::Target::Root;
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

/* Resolve `path` into `slot`: the layer's translation first, then the
 * namespace's resolve of the VFS path it answered. False when the path is
 * relative and there is no current directory, when it is the synthetic root --
 * which no volume answers for, and which only the calls in names_the_root
 * handle -- when the volume is unknown, or when the capability did not
 * arrive. */
bool resolve_target(char const *path, uint32_t length, seL4_CPtr slot,
                    Target &out) noexcept
{
    char full[kPathCapacity];
    view::Translation where{};
    if (!posix_path(path, length, full, where) || where.target != view::Target::Vfs) {
        return false;
    }
    aegir::vfs::Namespace &space = namespace_port();
    if (!space.valid()) {
        return false;
    }
    aegir::vfs::Namespace::Resolved resolved{};
    if (!space.resolve(full, where.length, slot, resolved) ||
        resolved.rest_length > kPathCapacity) {
        return false;
    }
    copy_text(out.rest, resolved.rest, resolved.rest_length);
    out.rest_length = resolved.rest_length;
    out.volume = slot;
    /* The volume part of the translated path, as written: two resolves of the
     * same volume through different aliases answer with different aliases, so
     * only the spelling can say whether two paths share one volume -- and
     * rename must not cross volumes. */
    uint32_t name_length = 0;
    while (name_length < where.length && full[name_length] != ':') {
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
                          aegir::bootstrap::endpoint_depth());
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
    /* The synthetic root: a directory with no volume and no path, whose
     * listing is the namespace's own (specs/posix.md). */
    bool root;
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
    entry.root = false;
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

/* The synthetic root as a directory fd: no volume, no path, and no capability
 * to give back at close. Its entries are the namespace's -- every volume and
 * every binding, one each -- which is what getdents reads for it
 * (specs/posix.md). */
int install_root() noexcept
{
    int const fd = alloc_fd();
    if (fd == kNoFd) {
        return -EMFILE;
    }
    Entry &entry = g_entries[fd];
    entry.root = true;
    entry.directory = true;
    entry.readable = false;
    entry.writable = false;
    entry.volume = 0;
    entry.handle = 0;
    entry.offset = 0;
    entry.index = 0;
    entry.path_length = 0;
    return fd;
}

void fill_kstat(Kstat *out, uint64_t kind, uint64_t size, uint64_t mtime) noexcept
{
    *out = Kstat{};
    out->st_mode = static_cast<mode_t>(
        kind == aegir::volume::kKindDir ? S_IFDIR | 0755 : S_IFREG | 0644);
    out->st_nlink = 1;
    out->st_ino = 1;
    out->st_size = size;
    out->st_blksize = 4096;
    out->st_blocks = (size + 511) / 512;
    /* The one time the filesystem reports is the last-write time; the other
     * two mirror it rather than lying with zero (specs/fat.md's Times). */
    out->st_atime_sec = static_cast<long>(mtime);
    out->st_mtime_sec = static_cast<long>(mtime);
    out->st_ctime_sec = static_cast<long>(mtime);
}

/* One file's kind and size into a kstat, by path or by the fd that names it.
 * The path form answers the synthetic root first: `/` is a directory, and it
 * has no volume to stat (specs/posix.md). */
bool stat_target(char const *path, uint32_t length, Kstat *out) noexcept
{
    if (names_the_root(path, length)) {
        fill_kstat(out, aegir::volume::kKindDir, 0, 0);
        return true;
    }
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
            fill_kstat(out, info.kind, info.size, info.mtime);
        }
    }
    empty_slot(slot);
    return ok;
}

bool stat_entry(Entry const &entry, Kstat *out) noexcept
{
    /* An fd on the synthetic root fstats as the directory it is. */
    if (entry.root) {
        fill_kstat(out, aegir::volume::kKindDir, 0, 0);
        return true;
    }
    aegir::vfs::Volume::Info info{};
    if (!aegir::vfs::Volume(entry.volume).stat(entry.path, entry.path_length, info)) {
        return false;
    }
    fill_kstat(out, info.kind, info.size, info.mtime);
    return true;
}

}  // namespace

void adopt(aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch) noexcept
{
    g_allocator = &allocator;
    g_scratch = &scratch;
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
        /* A directory fd as the anchor is not answered yet: the calls libc++'s
         * filesystem makes in the common paths pass AT_FDCWD. */
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
    if (names_the_root(path, text_length(path))) {
        /* The synthetic root (specs/posix.md): a directory to browse -- which
         * is what `opendir("/")` opens it as -- and not a file to open. */
        if ((flags & O_DIRECTORY) == 0) {
            return -EISDIR;
        }
        return install_root();
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
    /* A read open takes a read handle, and a union answers with a capability to
     * the member it chose and that member's own path, so the fd names the
     * member directly and every read is one call rather than one through the
     * union and one on (specs/vfs.md's scaling path). A filesystem that serves
     * no read handles answers zero, and the fd falls back to path reads
     * (volume::kMethodReadHandle is optional; the write side is not). */
    seL4_CPtr const member = take_slot();
    uint64_t handle = 0;
    char member_path[kPathCapacity];
    uint32_t member_path_length = 0;
    bool bypassed = false;
    bool const opened = member != 0 &&
                        volume.open_read(target.rest, target.rest_length, handle, member,
                                         member_path, kPathCapacity, &member_path_length,
                                         &bypassed);
    if (!opened) {
        if (member != 0) {
            give_slot(member);
        }
        give_slot(slot);
        return -EIO;
    }
    if (handle == 0) {
        give_slot(member);
        return install(slot, target, 0, false, true, false);
    }
    if (bypassed) {
        /* The member's cap and its own path replace the union's. */
        give_slot(slot);
        Target member_target = target;
        copy_text(member_target.rest, member_path, member_path_length);
        member_target.rest_length = member_path_length;
        return install(member, member_target, handle, false, true, false);
    }
    give_slot(member);
    return install(slot, target, handle, false, true, false);
}

/* The redirected standard streams (specs/shell.md): a read open refuses a name
 * that is not there; a write open creates it and cuts it to nothing first, the
 * Amiga's `>` (append is the shell's `>>`, which opens without truncation --
 * this is the one truncating form, so it is named for what it does). */
long open_for_read(char const *path) noexcept
{
    return openat(AT_FDCWD, path, O_RDONLY, 0);
}

long open_for_write(char const *path) noexcept
{
    return openat(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

long close(int fd) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr) {
        return -EBADF;
    }
    if (entry->handle != 0) {
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
        bool ok = false;
        if (entry->handle != 0) {
            ok = volume.read_handle(entry->handle, entry->offset, count - total, bytes);
        }
        if (!ok) {
            /* No handle, or a filesystem that does not serve one: the path
             * read is the fallback, and the only shape a read-only volume
             * that predates handles would answer. */
            ok = volume.read(entry->path, entry->path_length, entry->offset,
                             count - total, bytes);
        }
        if (!ok || bytes.count == 0) {
            if (!ok) {
                break;
            }
            if (bytes.eof) {
                break;
            }
            /* A pipe with nothing more yet (specs/pipe.md): return what this
             * read has, a short read -- a pipe is a stream, not a file whose
             * full count can be waited for -- and only wait when it has
             * nothing at all. A regular file never answers this: a read past
             * its end sets the flag. */
            if (total > 0) {
                break;
            }
            seL4_Yield();
            continue;
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

/* pread64: read at an explicit offset without moving the fd's cursor, which is
 * what `pread` promises. The cursor is saved and restored around the ordinary
 * read. */
long pread(int fd, void *buffer, size_t count, long offset) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr) {
        return -EBADF;
    }
    if (offset < 0) {
        return -EINVAL;
    }
    uint64_t const saved = entry->offset;
    entry->offset = static_cast<uint64_t>(offset);
    long const result = read(fd, buffer, count);
    entry->offset = saved;
    return result;
}

/* Read into a caller's frame (aegir/volume.h's read-frame): one call per 4 KiB
 * page rather than the several an inline read needs, and the bytes never cross
 * a message. The fd must carry a read handle -- the shape an open for reading
 * takes -- because the method names one. */
long read_frame(int fd, uint64_t offset, uint64_t frame_offset, uint64_t length,
                seL4_CPtr frame, uint32_t frame_bits) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr || !entry->readable || entry->directory || entry->handle == 0 ||
        frame == 0) {
        return -EBADF;
    }
    uint64_t count = 0;
    bool eof = false;
    if (!aegir::vfs::Volume(entry->volume)
             .read_frame(entry->handle, offset, length, frame_offset, frame, frame_bits,
                         count, eof)) {
        return -EIO;
    }
    return static_cast<long>(count);
}

long write(int fd, void const *buffer, size_t count) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr || !entry->writable || entry->handle == 0 ||
        buffer == nullptr) {
        return -EBADF;
    }
    auto const *bytes_in = static_cast<char const *>(buffer);
    /* A write larger than one envelope goes a page at a time through a frame of
     * the runtime's own (write-frame), where an inline write sends one
     * 936-byte envelope each: the bytes are copied into the frame and the
     * filesystem writes it whole. A volume that does not serve write-frame --
     * a pipe or NIL, say -- refuses the first call and the inline path below
     * takes over, so nothing regresses. */
    if (count > aegir::volume::kWriteMax && ensure_write_frame()) {
        size_t total = 0;
        while (total < count) {
            uint32_t const chunk = static_cast<uint32_t>(
                count - total < aegir::volume::kFrameBytes ? count - total
                                                           : aegir::volume::kFrameBytes);
            __builtin_memcpy(g_write_frame_map, bytes_in + total, chunk);
            uint64_t written = 0;
            if (!aegir::vfs::Volume(entry->volume)
                     .write_frame(entry->handle, chunk, 0, g_write_frame, written) ||
                written == 0) {
                break;
            }
            total += written;
        }
        if (total > 0) {
            /* A write moves the descriptor's cursor (POSIX), which is the mirror
             * lseek answers from: the volume's handle keeps its own place, so
             * this is bookkeeping the layer owes the program. Without it,
             * lseek(fd, 0, SEEK_CUR) after a write answered 0 -- measured by the
             * file sub-arc's client, whose seventh check is exactly that
             * (specs/posix.md). pwrite is the one that must not move it. */
            entry->offset += total;
            return static_cast<long>(total);
        }
    }
    size_t total = 0;
    while (total < count) {
        uint32_t const chunk = static_cast<uint32_t>(
            count - total < aegir::volume::kWriteMax ? count - total
                                                     : aegir::volume::kWriteMax);
        uint64_t written = 0;
        if (!aegir::vfs::Volume(entry->volume)
                 .write(entry->handle, bytes_in + total, chunk, &written)) {
            break;
        }
        if (written == 0) {
            /* A full pipe (specs/pipe.md): the buffer is the grant, and a
             * write that could not take its bytes waits for the reader to
             * drain and asks again. */
            seL4_Yield();
            continue;
        }
        total += written;
    }
    entry->offset += total;
    return static_cast<long>(total);
}

/* pwrite64: write at an explicit offset without moving the fd's cursor. */
long pwrite(int fd, void const *buffer, size_t count, long offset) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr) {
        return -EBADF;
    }
    if (offset < 0) {
        return -EINVAL;
    }
    uint64_t const saved = entry->offset;
    entry->offset = static_cast<uint64_t>(offset);
    long const result = write(fd, buffer, count);
    entry->offset = saved;
    return result;
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

namespace {

/* One directory entry in the kernel's shape, which musl's readdir reads
 * straight into its own struct dirent. False when the caller's buffer is full:
 * nothing is written and the listing cursor does not move, so the next call
 * carries on where this one stopped. */
bool put_dirent(uint8_t *out, size_t *used, size_t count, uint64_t index,
                char const *name, uint32_t name_length, uint8_t kind) noexcept
{
    uint32_t const record = (kDirentHeader + name_length + 1 + 7) & ~7u;
    if (*used + record > count) {
        return false;
    }
    auto *dirent = reinterpret_cast<Dirent64 *>(out + *used);
    dirent->d_ino = index + 1;
    dirent->d_off = static_cast<int64_t>(index + 1);
    dirent->d_reclen = static_cast<uint16_t>(record);
    dirent->d_type = kind;
    copy_text(dirent->d_name, name, name_length);
    dirent->d_name[name_length] = '\0';
    *used += record;
    return true;
}

}  // namespace

long getdents(int fd, void *buffer, size_t count) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr || !entry->directory || buffer == nullptr) {
        return -EBADF;
    }
    auto *out = static_cast<uint8_t *>(buffer);
    size_t used = 0;
    /* The synthetic root lists the namespace's own names, so its two counts are
     * asked once for the whole call (specs/posix.md). */
    uint64_t volumes = 0;
    uint64_t bindings = 0;
    aegir::vfs::Namespace &space = namespace_port();
    if (entry->root &&
        (!space.valid() || !space.volume_count(volumes) || !space.bind_count(bindings))) {
        volumes = 0;
        bindings = 0;
    }
    for (;;) {
        if (entry->root) {
            /* Every volume and every binding hangs off `/` as one entry, in the
             * order the namespace lists them and under the name the volume
             * registered. A volume that says it has no directory (kFlagNoDir:
             * NIL:) is listed and is not one to browse, which is what the type
             * says here (specs/posix.md). */
            nmspace::Row row{};
            nmspace::BindingRow binding{};
            char const *name = nullptr;
            uint8_t kind = kDirentDirectory;
            if (entry->index < volumes) {
                if (!space.describe(entry->index, row)) {
                    break;
                }
                name = row.name;
                if ((row.flags & nmspace::kFlagNoDir) != 0) {
                    kind = kDirentRegular;
                }
            } else if (entry->index - volumes < bindings) {
                if (!space.bind_describe(entry->index - volumes, binding)) {
                    break;
                }
                name = binding.name;
            } else {
                break; /* the end of the root */
            }
            if (!put_dirent(out, &used, count, entry->index, name, text_length(name),
                            kind)) {
                break; /* the caller's buffer is full; the next call continues */
            }
            entry->index += 1;
            continue;
        }
        /* `listed.name` points into the Volume, so it must outlive the copy. */
        aegir::vfs::Volume volume(entry->volume);
        aegir::vfs::Volume::Entry listed{};
        if (!volume.list(entry->path, entry->path_length, entry->index, listed)) {
            break; /* end of directory (or a refusal, which reads as one) */
        }
        uint8_t const kind = listed.kind == aegir::volume::kKindDir ? kDirentDirectory
                                                                   : kDirentRegular;
        if (!put_dirent(out, &used, count, entry->index, listed.name,
                        listed.name_length, kind)) {
            break; /* the caller's buffer is full; the next call continues */
        }
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

/* ---- attributes ---- */

/* The metadata protocol's status word as an errno. A filesystem with no
 * attributes answers kUnsupported, which is EOPNOTSUPP; one with attributes
 * but not this name answers kNotFound, which for an attribute is ENODATA. */
int status_errno(uint64_t status) noexcept
{
    switch (status) {
    case aegir::metadata::kOk:
        return 0;
    case aegir::metadata::kUnsupported:
        return EOPNOTSUPP;
    case aegir::metadata::kNotFound:
        return ENODATA;
    case aegir::metadata::kInvalidName:
        return ERANGE;
    case aegir::metadata::kReadOnly:
        return EROFS;
    case aegir::metadata::kNoSpace:
        return ENOSPC;
    case aegir::metadata::kNotADirectory:
        return ENOTDIR;
    case aegir::metadata::kIsADirectory:
        return EISDIR;
    case aegir::metadata::kPermission:
        return EACCES;
    default:
        return EIO;
    }
}

/* The file mode (the AmigaDOS Protect). Only the owner or the system class may
 * change it -- the filesystem enforces that and answers kPermission when it
 * refuses; a filesystem with no modes answers kUnsupported, EOPNOTSUPP. */
long fchmodat(int dfd, char const *path, int mode) noexcept
{
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    if (dfd != AT_FDCWD) {
        return -EINVAL; /* no *at anchor yet */
    }
    if (path == nullptr) {
        return -EFAULT;
    }
    seL4_CPtr const slot = transient_slot();
    if (slot == 0) {
        return -EMFILE;
    }
    long result = -ENOENT;
    Target target{};
    if (resolve_target(path, text_length(path), slot, target)) {
        uint64_t const status = aegir::vfs::Volume(target.volume)
                                    .protect(target.rest, target.rest_length,
                                             static_cast<uint32_t>(mode) & 07777u);
        result = status == aegir::metadata::kOk ? 0 : -status_errno(status);
    }
    empty_slot(slot);
    return result;
}

long fchmod(int fd, int mode) noexcept
{
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    Entry *entry = entry_for(fd);
    if (entry == nullptr) {
        return -EBADF;
    }
    uint64_t const status = aegir::vfs::Volume(entry->volume)
                                .protect(entry->path, entry->path_length,
                                         static_cast<uint32_t>(mode) & 07777u);
    return status == aegir::metadata::kOk ? 0 : -status_errno(status);
}

/* Write a value of any size: one attr_write carries at most kAttrDataMax, so
 * a larger value goes at successive offsets. */
long write_attribute(aegir::vfs::Volume volume, char const *rest,
                     uint32_t rest_length, char const *name, size_t size,
                     void const *value) noexcept
{
    auto const *bytes = static_cast<uint8_t const *>(value);
    uint64_t offset = 0;
    for (;;) {
        if (offset >= size) {
            break;
        }
        uint32_t const chunk = static_cast<uint32_t>(
            size - offset < aegir::metadata::kAttrDataMax
                ? size - offset
                : aegir::metadata::kAttrDataMax);
        uint64_t const status = volume.attr_write(
            rest, rest_length, name, text_length(name), aegir::metadata::kTypeRaw,
            offset, bytes + offset, chunk);
        if (status != aegir::metadata::kOk) {
            return -status_errno(status);
        }
        offset += chunk;
    }
    if (size == 0) {
        uint64_t const status = volume.attr_write(
            rest, rest_length, name, text_length(name), aegir::metadata::kTypeRaw,
            0, nullptr, 0);
        if (status != aegir::metadata::kOk) {
            return -status_errno(status);
        }
    }
    return 0;
}

long read_attribute(aegir::vfs::Volume volume, char const *rest,
                    uint32_t rest_length, char const *name, void *value,
                    size_t size) noexcept
{
    uint32_t type = 0;
    uint64_t value_size = 0;
    uint64_t status = volume.attr_stat(rest, rest_length, name, text_length(name),
                                       type, value_size);
    if (status != aegir::metadata::kOk) {
        return -status_errno(status);
    }
    if (size == 0) {
        return static_cast<long>(value_size);
    }
    if (size < value_size) {
        return -ERANGE;
    }
    auto *out = static_cast<uint8_t *>(value);
    uint64_t offset = 0;
    while (offset < value_size) {
        uint32_t got = static_cast<uint32_t>(
            value_size - offset < aegir::metadata::kAttrDataMax
                ? value_size - offset
                : aegir::metadata::kAttrDataMax);
        status = volume.attr_read(rest, rest_length, name, text_length(name),
                                  offset, out + offset, got);
        if (status != aegir::metadata::kOk) {
            return -status_errno(status);
        }
        if (got == 0) {
            break;
        }
        offset += got;
    }
    return static_cast<long>(value_size);
}

long list_attributes(aegir::vfs::Volume volume, char const *rest,
                     uint32_t rest_length, char *list, size_t size) noexcept
{
    char name[aegir::metadata::kAttrNameMax];
    uint64_t total = 0;
    for (uint64_t index = 0;; ++index) {
        uint32_t name_length = 0;
        uint32_t type = 0;
        uint64_t attribute_size = 0;
        uint64_t const status =
            volume.attr_list(rest, rest_length, index, name, name_length, type,
                             attribute_size);
        if (status == aegir::metadata::kNotFound) {
            break;
        }
        if (status != aegir::metadata::kOk) {
            return -status_errno(status);
        }
        total += name_length + 1;
    }
    if (size == 0) {
        return static_cast<long>(total);
    }
    if (size < total) {
        return -ERANGE;
    }
    size_t used = 0;
    for (uint64_t index = 0;; ++index) {
        uint32_t name_length = 0;
        uint32_t type = 0;
        uint64_t attribute_size = 0;
        uint64_t const status =
            volume.attr_list(rest, rest_length, index, name, name_length, type,
                             attribute_size);
        if (status != aegir::metadata::kOk) {
            break;
        }
        for (uint32_t i = 0; i < name_length; ++i) {
            list[used + i] = name[i];
        }
        list[used + name_length] = '\0';
        used += name_length + 1;
    }
    return static_cast<long>(used);
}

long setxattr(char const *path, char const *name, void const *value, size_t size,
              int flags) noexcept
{
    static_cast<void>(flags);
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    if (path == nullptr || name == nullptr || (value == nullptr && size != 0)) {
        return -EFAULT;
    }
    seL4_CPtr const slot = transient_slot();
    if (slot == 0) {
        return -EMFILE;
    }
    long result = -ENOENT;
    Target target{};
    if (resolve_target(path, text_length(path), slot, target)) {
        result = write_attribute(aegir::vfs::Volume(target.volume), target.rest,
                                 target.rest_length, name, size, value);
    }
    empty_slot(slot);
    return result;
}

long lsetxattr(char const *path, char const *name, void const *value, size_t size,
               int flags) noexcept
{
    return setxattr(path, name, value, size, flags);
}

long fsetxattr(int fd, char const *name, void const *value, size_t size,
               int flags) noexcept
{
    static_cast<void>(flags);
    Entry *entry = entry_for(fd);
    if (entry == nullptr || name == nullptr || (value == nullptr && size != 0)) {
        return -EBADF;
    }
    return write_attribute(aegir::vfs::Volume(entry->volume), entry->path,
                           entry->path_length, name, size, value);
}

long getxattr(char const *path, char const *name, void *value, size_t size) noexcept
{
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    if (path == nullptr || name == nullptr || (value == nullptr && size != 0)) {
        return -EFAULT;
    }
    seL4_CPtr const slot = transient_slot();
    if (slot == 0) {
        return -EMFILE;
    }
    long result = -ENOENT;
    Target target{};
    if (resolve_target(path, text_length(path), slot, target)) {
        result = read_attribute(aegir::vfs::Volume(target.volume), target.rest,
                                target.rest_length, name, value, size);
    }
    empty_slot(slot);
    return result;
}

long lgetxattr(char const *path, char const *name, void *value, size_t size) noexcept
{
    return getxattr(path, name, value, size);
}

long fgetxattr(int fd, char const *name, void *value, size_t size) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr || name == nullptr || (value == nullptr && size != 0)) {
        return -EBADF;
    }
    return read_attribute(aegir::vfs::Volume(entry->volume), entry->path,
                          entry->path_length, name, value, size);
}

long listxattr(char const *path, char *list, size_t size) noexcept
{
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    if (path == nullptr || (list == nullptr && size != 0)) {
        return -EFAULT;
    }
    seL4_CPtr const slot = transient_slot();
    if (slot == 0) {
        return -EMFILE;
    }
    long result = -ENOENT;
    Target target{};
    if (resolve_target(path, text_length(path), slot, target)) {
        result = list_attributes(aegir::vfs::Volume(target.volume), target.rest,
                                 target.rest_length, list, size);
    }
    empty_slot(slot);
    return result;
}

long llistxattr(char const *path, char *list, size_t size) noexcept
{
    return listxattr(path, list, size);
}

long flistxattr(int fd, char *list, size_t size) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr || (list == nullptr && size != 0)) {
        return -EBADF;
    }
    return list_attributes(aegir::vfs::Volume(entry->volume), entry->path,
                           entry->path_length, list, size);
}

long removexattr(char const *path, char const *name) noexcept
{
    if (g_allocator == nullptr) {
        return -ENOSYS;
    }
    if (path == nullptr || name == nullptr) {
        return -EFAULT;
    }
    seL4_CPtr const slot = transient_slot();
    if (slot == 0) {
        return -EMFILE;
    }
    long result = -ENOENT;
    Target target{};
    if (resolve_target(path, text_length(path), slot, target)) {
        uint64_t const status =
            aegir::vfs::Volume(target.volume)
                .attr_remove(target.rest, target.rest_length, name,
                             text_length(name));
        result = status == aegir::metadata::kOk ? 0 : -status_errno(status);
    }
    empty_slot(slot);
    return result;
}

long lremovexattr(char const *path, char const *name) noexcept
{
    return removexattr(path, name);
}

long fremovexattr(int fd, char const *name) noexcept
{
    Entry *entry = entry_for(fd);
    if (entry == nullptr || name == nullptr) {
        return -EBADF;
    }
    uint64_t const status = aegir::vfs::Volume(entry->volume)
                                .attr_remove(entry->path, entry->path_length,
                                             name, text_length(name));
    return status == aegir::metadata::kOk ? 0 : -status_errno(status);
}

/* chdir, in the view (specs/posix.md): a POSIX path is translated to the VFS
 * path the layer keeps, `/` is the root state -- no VFS path names it -- and a
 * native path is stored as written. So the one current directory a native
 * program and aegir::environment read stays a VFS path, and the view derives
 * its own form of it. */
long chdir(char const *path) noexcept
{
    if (path == nullptr) {
        return -EFAULT;
    }
    char full[kPathCapacity];
    view::Translation where{};
    if (!posix_path(path, text_length(path), full, where)) {
        return -ENOENT;
    }
    if (where.target == view::Target::Root) {
        set_root_dir();
        return 0;
    }
    /* The grammar the caller set the directory with is the grammar `getcwd`
     * answers it in (specs/posix.md): a native path leaves Aegir's own, which
     * is the form std::filesystem and the compiler read. */
    set_current_dir(full, where.length,
                    !view::detail::names_a_volume(path, text_length(path)));
    return 0;
}

/* getcwd, in the view (specs/posix.md): the answer is in the grammar the
 * directory was set in -- `/` and `/Sys/Tests` for a POSIX chdir, `Sys:Tests`
 * for a native one or for the directory this process inherited, which is the
 * form std::filesystem and the compiler compose with (presented_cwd). The name
 * is the one the path carries, as written, so a resolve through an alias
 * presents the alias -- the same rule a listing follows. */
long getcwd(char *buffer, size_t size) noexcept
{
    if (buffer == nullptr) {
        return -EFAULT;
    }
    char seen[kPathCapacity];
    uint32_t length = 0;
    char const *const presented = presented_cwd(seen, &length);
    if (presented == nullptr) {
        return -ENOENT;
    }
    if (size < static_cast<size_t>(length) + 1) {
        return -ERANGE;
    }
    copy_text(buffer, presented, length);
    buffer[length] = '\0';
    return static_cast<long>(length) + 1;
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

/* sendfile: copy from a readable fd to a writable one, which is how libc++'s
 * copy_file moves a file on Linux. The input is read at its cursor (or the
 * offset the caller named, which is not advanced), the output at its own
 * handle's cursor. A short return is a filesystem refusal or the input's end,
 * the way the syscall reports; there is no SIGPIPE to raise. */
long sendfile(int out_fd, int in_fd, long *offset, size_t count) noexcept
{
    Entry *const in = entry_for(in_fd);
    Entry *const out = entry_for(out_fd);
    if (in == nullptr || !in->readable || in->directory) {
        return -EBADF;
    }
    if (out == nullptr || !out->writable || out->handle == 0) {
        return -EBADF;
    }
    uint64_t position = in->offset;
    if (offset != nullptr) {
        position = static_cast<uint64_t>(*offset);
    }
    /* Frame-to-frame (specs/vfs.md's scaling path): a page is read straight
     * into a frame of ours by the source filesystem and written whole from it
     * by the destination -- one call a page where the inline path needs
     * several, and never through a message. The frame is claimed for the copy
     * and given back after. A source with no read handle, or a filesystem that
     * serves no frames, fails the first read and the inline path takes over. */
    size_t total = 0;
    if (in->handle != 0 && out->handle != 0 && g_allocator != nullptr && count > 0) {
        aegir::mem::Account account{"copy", 0, 0, 0};
        seL4_Error error = seL4_NoError;
        void *cookie = nullptr;
        seL4_CPtr const frame = g_allocator->alloc_object(
            seL4_RISCV_4K_Page, seL4_PageBits, account, &error, &cookie);
        if (frame != 0) {
            aegir::vfs::Volume source(in->volume);
            aegir::vfs::Volume dest(out->volume);
            while (total < count) {
                uint64_t got = 0;
                bool eof = false;
                if (!source.read_frame(in->handle, position, aegir::volume::kFrameBytes, 0,
                                       frame, aegir::volume::kFrameBitsMin, got, eof) ||
                    got == 0) {
                    break;
                }
                uint64_t written = 0;
                if (!dest.write_frame(out->handle, got, 0, frame, written) ||
                    written == 0) {
                    break;
                }
                position += written;
                total += static_cast<size_t>(written);
                if (eof) {
                    break;
                }
            }
            g_allocator->free_object(cookie, seL4_PageBits);
        }
    }
    if (total > 0) {
        if (offset != nullptr) {
            *offset = static_cast<long>(position);
        } else {
            in->offset = position;
        }
        return static_cast<long>(total);
    }
    /* The inline path: a copy on a pipe, a source with no read handle, or a
     * filesystem that serves no frames. */
    total = 0;
    while (total < count) {
        aegir::vfs::Volume::Bytes bytes{};
        uint32_t const wanted = static_cast<uint32_t>(
            count - total < aegir::volume::kReadMax ? count - total
                                                    : aegir::volume::kReadMax);
        if (!aegir::vfs::Volume(in->volume)
                 .read(in->path, in->path_length, position, wanted, bytes) ||
            bytes.count == 0) {
            break;
        }
        uint64_t written = 0;
        if (!aegir::vfs::Volume(out->volume)
                 .write(out->handle, bytes.data, static_cast<uint32_t>(bytes.count),
                        &written) ||
            written == 0) {
            break;
        }
        position += bytes.count;
        total += bytes.count;
        if (bytes.eof) {
            break;
        }
    }
    if (offset != nullptr) {
        *offset = static_cast<long>(position);
    } else {
        in->offset = position;
    }
    return static_cast<long>(total);
}

}  // namespace aegir::posix::files

/* The current directory's two ends, for aegir::environment -- which is the
 * hosted C++ face of the same state chdir/getcwd answer. Plain C so the
 * libc++-facing translation unit can declare them (specs/environment.md). */
extern "C" char const *aegir_posix_current_dir(uint32_t *length) noexcept
{
    return aegir::posix::files::current_dir(length);
}

extern "C" int aegir_posix_set_current_dir(char const *path, uint32_t length) noexcept
{
    if (path == nullptr && length != 0) {
        return -1;
    }
    if (length > aegir::posix::files::kPathCapacity) {
        return -1;
    }
    /* A native caller set it, so `getcwd` answers it in Aegir's own grammar
     * (specs/posix.md): this is the door aegir::environment and a native
     * std::filesystem::current_path(path) come through. */
    aegir::posix::files::set_current_dir(path, length, false);
    return 0;
}

/* The program directory, for aegir::environment: a plain-C bridge, the same
 * shape as the current directory's, so the libc++-facing unit can declare it
 * (specs/environment.md). */
extern "C" char const *aegir_posix_program_dir(uint32_t *length) noexcept
{
    return aegir::posix::files::program_dir(length);
}
