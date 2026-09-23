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
long read(int fd, void *buffer, size_t count) noexcept;
long write(int fd, void const *buffer, size_t count) noexcept;
long lseek(int fd, long offset, int whence) noexcept;
long getdents(int fd, void *buffer, size_t count) noexcept;
long mkdirat(int dfd, char const *path, int mode) noexcept;
long unlinkat(int dfd, char const *path, int flags) noexcept;
long chdir(char const *path) noexcept;
long getcwd(char *buffer, size_t size) noexcept;
long fcntl(int fd, int command, long argument) noexcept;

}  // namespace aegir::heap::files

#endif  // AEGIR_HEAP_FILES_H
