/*
 * aegir-posix: the file surface (specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The POSIX layer musl's filesystem functions and std::filesystem reach
 * (specs/cxx.md step 5, specs/posix.md). The dispatcher in aegir-heap's
 * heap.cc owns the switch over musl's syscall numbers; this header is the
 * handful of functions it calls for the filesystem. They live here rather than
 * in the heap because they are the personality and the heap is the allocator:
 * this is the one place a hosted runtime touches seL4 -- it resolves Aegir
 * paths through aegir::vfs -- and the layer grows in one library
 * (specs/posix.md's boundary, specs/clang-on-aegir.md's Phase 2).
 *
 * Plain types only, so heap.cc can include this without an seL4 header of its
 * own, with one exception: read_frame hands a page *capability* over, which has
 * no plain-C shape (aegir/volume.h, `seL4_CPtr`). The free functions are named
 * after the syscall they answer, not the libc function, because that is the
 * layer they intercept.
 */

#ifndef AEGIR_POSIX_FILES_H
#define AEGIR_POSIX_FILES_H

#include <aegir/volume.h>
#include <stddef.h>
#include <stdint.h>

namespace aegir::mem {
class Allocator;
class Scratch;
}

namespace aegir::posix::files {

/** Hand the file layer the allocator its capability slots come from and the
 *  window it maps a bulk write's frame through. Called from heap::init; before
 *  it, every file syscall is refused. */
void adopt(aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch) noexcept;

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
/* Positional reads and writes (pread64/pwrite64): read or write at an explicit
 * offset, leaving the fd's own cursor where it was. LLVM's MemoryBuffer reads a
 * file it does not map through pread, so this is the first call the on-device
 * compiler needed that the surface did not answer (specs/clang-on-aegir.md). */
long pread(int fd, void *buffer, size_t count, long offset) noexcept;
long pwrite(int fd, void const *buffer, size_t count, long offset) noexcept;
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

/** Read `length` bytes of the file open as `fd`, from its `offset`, into
 *  `frame` -- a 4 KiB page capability of the caller's -- starting
 *  `frame_offset` bytes in. This is the volume protocol's read-frame
 *  (aegir/volume.h): the filesystem maps the frame for the one call and the
 *  bytes never cross a message, so a program image is read straight into the
 *  frames the child will hold (specs/director.md's spawn path). It has no libc
 *  shape -- it hands a capability over -- so it is named rather than reached
 *  through musl, the way every other call here is. Returns the number of bytes
 *  read, or a negative errno -- EIO when the volume refuses, EBADF when the fd
 *  is not a readable file with a handle. */
long read_frame(int fd, uint64_t offset, uint64_t frame_offset, uint64_t length,
                seL4_CPtr frame,
                uint32_t frame_bits = aegir::volume::kFrameBitsMin) noexcept;

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

}  // namespace aegir::posix::files

#endif  // AEGIR_POSIX_FILES_H
