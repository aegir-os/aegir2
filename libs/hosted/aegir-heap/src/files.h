/*
 * The runtime's file calls: the POSIX layer musl's filesystem functions and
 * std::filesystem reach (specs/cxx.md step 5).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The dispatcher in heap.cc owns the switch over musl's syscall numbers; this
 * header is the handful of functions it calls for the filesystem. They live in
 * their own translation unit because this is the one place a hosted runtime
 * touches seL4 -- it resolves Aegir paths through aegir::vfs -- and keeping it
 * apart from heap.cc's musl-facing C leaves each side of the seL4/libc++
 * boundary in one translation unit (specs/userland.md).
 *
 * Plain types only, so heap.cc can include this without an seL4 header of its
 * own; the free functions are named after the syscall they answer, not the
 * libc function, because that is the layer they intercept.
 */

#ifndef AEGIR_HEAP_FILES_H
#define AEGIR_HEAP_FILES_H

#include <stddef.h>
#include <stdint.h>

namespace aegir::mem {
class Allocator;
}

namespace aegir::heap::files {

/** Hand the file layer the allocator its capability slots come from. Called
 *  from heap::init; before it, every file syscall is refused. */
void adopt(aegir::mem::Allocator &allocator) noexcept;

/* One function per syscall the filesystem reaches, with the arguments musl
 * passes the kernel. Each answers a value or a negative errno the way a
 * syscall does. */
long newfstatat(int dfd, char const *path, void *buffer, int flags) noexcept;
long fstat(int fd, void *buffer) noexcept;
long openat(int dfd, char const *path, int flags, int mode) noexcept;
long close(int fd) noexcept;
/* Open a path as a redirected standard stream (specs/shell.md): fd 0 reads it
 * (refused when it is not there), fd 1 writes it (created and truncated).
 * Returns the fd, or a negative errno. heap.cc owns the routing; the flags are
 * this layer's, so the two agree on what "for reading" and "for writing" mean. */
long open_for_read(char const *path) noexcept;
long open_for_write(char const *path) noexcept;
long read(int fd, void *buffer, size_t count) noexcept;
long write(int fd, void const *buffer, size_t count) noexcept;
long lseek(int fd, long offset, int whence) noexcept;
long getdents(int fd, void *buffer, size_t count) noexcept;
long mkdirat(int dfd, char const *path, int mode) noexcept;
long unlinkat(int dfd, char const *path, int flags) noexcept;
long renameat2(int old_dfd, char const *old_path, int new_dfd, char const *new_path,
               unsigned flags) noexcept;
long truncate(char const *path, long length) noexcept;
long ftruncate(int fd, long length) noexcept;
/* The file mode (the AmigaDOS Protect): chmod's family maps to the volume
 * protocol's protect, which BFS answers and FAT refuses with EOPNOTSUPP.
 * SYS_fchmodat is the three-argument call (fchmodat2 carries the flags, and
 * is not this), so there is no flags argument here. */
long fchmodat(int dfd, char const *path, int mode) noexcept;
long fchmod(int fd, int mode) noexcept;
long chdir(char const *path) noexcept;
long getcwd(char *buffer, size_t size) noexcept;
long fcntl(int fd, int command, long argument) noexcept;
/* libc++'s copy_file is sendfile(out, in, nullptr, size) on Linux; without it
 * std::filesystem::copy fails with ENOSYS (specs/dos.md's copy command). */
long sendfile(int out_fd, int in_fd, long *offset, size_t count) noexcept;

/* The attribute calls (specs/bfs.md's metadata protocol). A filesystem that
 * has no attributes answers EOPNOTSUPP; one that has them but not this name
 * answers ENODATA. `l`-prefixed names are the symlink forms, which read the
 * same as the path forms on a filesystem with no symlinks. */
long setxattr(char const *path, char const *name, void const *value, size_t size,
              int flags) noexcept;
long lsetxattr(char const *path, char const *name, void const *value, size_t size,
               int flags) noexcept;
long fsetxattr(int fd, char const *name, void const *value, size_t size,
               int flags) noexcept;
long getxattr(char const *path, char const *name, void *value, size_t size) noexcept;
long lgetxattr(char const *path, char const *name, void *value, size_t size) noexcept;
long fgetxattr(int fd, char const *name, void *value, size_t size) noexcept;
long listxattr(char const *path, char *list, size_t size) noexcept;
long llistxattr(char const *path, char *list, size_t size) noexcept;
long flistxattr(int fd, char *list, size_t size) noexcept;
long removexattr(char const *path, char const *name) noexcept;
long lremovexattr(char const *path, char const *name) noexcept;
long fremovexattr(int fd, char const *name) noexcept;

}  // namespace aegir::heap::files

#endif  // AEGIR_HEAP_FILES_H
