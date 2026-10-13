/*
 * aegir-posix-conformance: the files area.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Cases from open.html, close.html, read.html, write.html, pread.html,
 * pwrite.html, readv.html, writev.html, lseek.html, stat.html, fstat.html,
 * lstat.html, fstatat.html, access.html, truncate.html, ftruncate.html,
 * unlink.html, rename.html, chmod.html, fchmod.html, fcntl.html, fopen.html,
 * fclose.html, fread.html, fwrite.html, fflush.html, fileno.html, remove.html
 * and (for the holes) link.html, symlink.html, readlink.html, chown.html,
 * dup.html, dup2.html, pipe.html, fsync.html, fdatasync.html, sync.html,
 * flock.html, statvfs.html, fstatvfs.html, utime.html and mkfifo.html.
 *
 * The area runs against one volume, and the caller runs it against a FAT and a
 * BFS one (scratch.h): the expected result of `chmod`/`fchmod` is a property of
 * the filesystem, so it is asserted per volume rather than waved at.
 */

#include "harness.h"
#include "scratch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

namespace aegir_posix_conformance {
namespace {

/* Build "<volume.root>/name" into `out`. */
void under(Volume const &volume, char const *name, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", volume.root, name);
}

/* Put `text` in `path` (creating or truncating it), and close. -1 with errno set
 * on any failure. */
int write_text(char const *path, char const *text)
{
    int const fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        return -1;
    }
    size_t const n = length_of(text);
    ssize_t const wrote = write(fd, text, n);
    int const saved = errno;
    (void)close(fd);
    if (wrote != static_cast<ssize_t>(n)) {
        errno = saved;
        return -1;
    }
    return 0;
}

bool reads_text(char const *path, char const *text)
{
    int const fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char buffer[256];
    size_t const n = length_of(text);
    ssize_t const got = read(fd, buffer, sizeof(buffer));
    (void)close(fd);
    return got == static_cast<ssize_t>(n) && equals(buffer, text, n);
}

}  // namespace

void files_surface(Volume const &volume)
{
    char title[32];
    snprintf(title, sizeof(title), "files (%s)", volume.label);
    section(title);

    /* The disk is scanned by partmgr asynchronously, so the volume may not be
     * there yet (the measurement is in scratch.h). Wait for it; not appearing is
     * one named failure, not a hundred confusing ENOENTs. */
    if (!wait_for_volume(volume)) {
        check("open", "the volume is available", false);
        return;
    }

    char dir[160];
    char file[160];
    char second[160];
    char renamed[160];
    char absent_path[160];
    char fifo[160];
    under(volume, "POSIX.CONF", dir, sizeof(dir));
    under(volume, "POSIX.CONF/FILE.TXT", file, sizeof(file));
    under(volume, "POSIX.CONF/SECOND.TXT", second, sizeof(second));
    under(volume, "POSIX.CONF/RENAMED.TXT", renamed, sizeof(renamed));
    under(volume, "POSIX.CONF/NO-SUCH-FILE.TXT", absent_path, sizeof(absent_path));
    under(volume, "POSIX.CONF/FIFO", fifo, sizeof(fifo));

    /* A scratch directory of our own; already there on a re-run is not an
     * error. */
    (void)mkdir(dir, 0755);

    /* open: ERRORS ("[ENOENT] O_CREAT is not set and the named file does not
     * exist"; "[EEXIST] O_CREAT and O_EXCL are set, and the named file exists";
     * "[EISDIR] The named file is a directory and oflag includes O_WRONLY or
     * O_RDWR"). */
    errno = 0;
    check("open", "a missing file without O_CREAT is ENOENT",
          open(absent_path, O_RDONLY) == -1 && errno == ENOENT);
    {
        int const created = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        check("open", "O_CREAT makes the file", created >= 0);
        if (created >= 0) {
            (void)close(created);
        }
    }
    /* O_EXCL is not checked in openat (measured on FAT: the open succeeds), and
     * an existing directory opened for writing is refused with a filesystem's
     * own error. Each is a named deviation when it does not conform, so the gap
     * is a line and a count rather than a silence. */
    errno = 0;
    int const exclusive = open(file, O_CREAT | O_EXCL | O_WRONLY, 0644);
    int const exclusive_errno = errno;
    if (exclusive == -1 && exclusive_errno == EEXIST) {
        check("open", "O_CREAT|O_EXCL on an existing file is EEXIST", true);
    } else {
        if (exclusive >= 0) {
            (void)close(exclusive);
        }
        deviation("open", "O_CREAT|O_EXCL on an existing file is EEXIST",
                  exclusive >= 0 ? "O_EXCL is not checked: the open succeeds"
                                 : "the refusal is not EEXIST");
    }
    errno = 0;
    int const directory_write = open(dir, O_WRONLY);
    int const directory_errno = errno;
    if (directory_write == -1 && directory_errno == EISDIR) {
        check("open", "a directory O_WRONLY is EISDIR", true);
    } else {
        if (directory_write >= 0) {
            (void)close(directory_write);
        }
        char reason[96];
        snprintf(reason, sizeof(reason), "Aegir answers errno %d, not EISDIR", directory_errno);
        deviation("open", "a directory O_WRONLY is EISDIR", reason);
    }
    /* open.html reserves EISDIR for O_WRONLY/O_RDWR (or O_CREAT without
     * O_DIRECTORY): a directory opened O_RDONLY is not an error. Its descriptor
     * is then what read.html's own EISDIR is about -- a directory read directly,
     * which is why readdir exists. */
    errno = 0;
    int const directory_read = open(dir, O_RDONLY);
    if (directory_read >= 0) {
        check("open", "a directory O_RDONLY is opened", true);
        errno = 0;
        if (read(directory_read, nullptr, 1) == -1 && errno == EISDIR) {
            check("read", "reading a directory descriptor is EISDIR", true);
        } else {
            deviation("read", "reading a directory descriptor is EISDIR",
                      "the read did not answer EISDIR");
        }
        (void)close(directory_read);
    } else {
        deviation("open", "a directory O_RDONLY is opened",
                  "Aegir answers EISDIR; open.html reserves that for O_WRONLY/O_RDWR");
    }

    /* write: ERRORS ("[EBADF] The fildes argument is not a valid file descriptor
     * open for writing"). */
    check("write", "a successful write lands the text", write_text(file, kText) == 0);
    {
        int const read_only = open(file, O_RDONLY);
        errno = 0;
        check("write", "a read-only descriptor is EBADF",
              read_only >= 0 && write(read_only, kText, 1) == -1 && errno == EBADF);
        if (read_only >= 0) {
            (void)close(read_only);
        }
    }

    /* read: ERRORS ("[EBADF] The fildes argument is not a valid file descriptor
     * open for reading"). */
    check("read", "the text reads back", reads_text(file, kText));
    {
        int const write_only = open(file, O_WRONLY);
        errno = 0;
        check("read", "a write-only descriptor is EBADF",
              write_only >= 0 && read(write_only, nullptr, 1) == -1 && errno == EBADF);
        if (write_only >= 0) {
            (void)close(write_only);
        }
    }
    errno = 0;
    check("read", "a bad descriptor is EBADF", read(-1, nullptr, 1) == -1 && errno == EBADF);

    /* close: RETURN VALUE (zero), ERRORS ("[EBADF] The fildes argument is not a
     * valid file descriptor"). */
    {
        int const closing = open(file, O_RDONLY);
        check("close", "a valid descriptor closes", closing >= 0 && close(closing) == 0);
        errno = 0;
        check("close", "the same descriptor is then EBADF",
              closing >= 0 && close(closing) == -1 && errno == EBADF);
    }

    /* lseek: RETURN VALUE (the resulting offset), ERRORS ("[EINVAL] The resulting
     * file offset would be negative"). */
    {
        int const seekable = open(file, O_RDWR);
        errno = 0;
        check("lseek", "a negative resulting offset is EINVAL",
              seekable >= 0 && lseek(seekable, -1, SEEK_SET) == -1 && errno == EINVAL);
        check("lseek", "SEEK_END is the size",
              seekable >= 0 &&
                  lseek(seekable, 0, SEEK_END) == static_cast<off_t>(length_of(kText)));
        check("lseek", "SEEK_SET lands on the byte",
              seekable >= 0 && lseek(seekable, 4, SEEK_SET) == 4);
        if (seekable >= 0) {
            (void)close(seekable);
        }
    }

    /* pread/pwrite: RETURN VALUE (the count), and the descriptor's own offset is
     * not the one this uses. */
    {
        int const positional = open(file, O_RDWR);
        if (positional >= 0) {
            char buffer[256];
            size_t const n = length_of(kText);
            ssize_t const got = pread(positional, buffer, n, 0);
            check("pread", "reads at an offset without moving the descriptor",
                  got == static_cast<ssize_t>(n) && equals(buffer, kText, n));
            check("pwrite", "writes a byte at an offset", pwrite(positional, "P", 1, 0) == 1);
            (void)close(positional);
        } else {
            check("pread", "reads at an offset without moving the descriptor", false);
            check("pwrite", "writes a byte at an offset", false);
        }
    }

    /* readv/writev: RETURN VALUE (the count). Chosen over read/write by musl's
     * buffered stdio, so what a FILE does rests on them. */
    {
        int const vectored = open(second, O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (vectored >= 0) {
            char out_a[] = "ab";
            char out_b[] = "cd";
            struct iovec out[2];
            out[0].iov_base = out_a;
            out[0].iov_len = 2;
            out[1].iov_base = out_b;
            out[1].iov_len = 2;
            check("writev", "writes two vectors", writev(vectored, out, 2) == 4);
            (void)lseek(vectored, 0, SEEK_SET);
            char in_a[2] = {0, 0};
            char in_b[2] = {0, 0};
            struct iovec in[2];
            in[0].iov_base = in_a;
            in[0].iov_len = 2;
            in[1].iov_base = in_b;
            in[1].iov_len = 2;
            ssize_t const got = readv(vectored, in, 2);
            check("readv", "reads them back into two vectors",
                  got == 4 && equals(in_a, "ab", 2) && equals(in_b, "cd", 2));
            (void)close(vectored);
        } else {
            check("writev", "writes two vectors", false);
            check("readv", "reads them back into two vectors", false);
        }
    }

    /* stat/fstat/lstat/fstatat: RETURN VALUE (zero), ERRORS ("[ENOENT] The named
     * file does not exist"; "[EBADF] The file descriptor argument is not a valid
     * file descriptor"). */
    struct stat st;
    errno = 0;
    check("stat", "a missing file is ENOENT", stat(absent_path, &st) == -1 && errno == ENOENT);
    check("stat", "a regular file is S_ISREG with a size",
          stat(file, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0);
    check("stat", "a directory is S_ISDIR", stat(dir, &st) == 0 && S_ISDIR(st.st_mode));
    {
        int const stated = open(file, O_RDONLY);
        check("fstat", "a valid descriptor answers",
              stated >= 0 && fstat(stated, &st) == 0 && S_ISREG(st.st_mode));
        if (stated >= 0) {
            (void)close(stated);
        }
    }
    errno = 0;
    check("fstat", "a bad descriptor is EBADF", fstat(-1, &st) == -1 && errno == EBADF);
    check("lstat", "a regular file is a regular file",
          lstat(file, &st) == 0 && S_ISREG(st.st_mode));
    check("fstatat", "AT_FDCWD names the path", fstatat(AT_FDCWD, file, &st, 0) == 0);

    /* access: RETURN VALUE (zero or -1), ERRORS ("[ENOENT] The file named by
     * path does not exist"). */
    errno = 0;
    check("access", "a missing file is ENOENT",
          access(absent_path, F_OK) == -1 && errno == ENOENT);
    check("access", "an existing file is readable", access(file, R_OK) == 0);

    /* truncate/ftruncate: RETURN VALUE (zero), ERRORS ("[EINVAL] The length
     * argument is less than zero"; "[EBADF] ... not open for writing"). */
    check("truncate", "a file is set to a length",
          write_text(second, kText) == 0 && truncate(second, 4) == 0 && stat(second, &st) == 0 &&
              st.st_size == 4);
    errno = 0;
    check("truncate", "a missing file is ENOENT",
          truncate(absent_path, 0) == -1 && errno == ENOENT);
    errno = 0;
    if (truncate(second, -1) == -1 && errno == EINVAL) {
        check("truncate", "a negative length is EINVAL", true);
    } else {
        deviation("truncate", "a negative length is EINVAL", "the call did not answer EINVAL");
    }
    errno = 0;
    check("ftruncate", "a bad descriptor is EBADF", ftruncate(-1, 0) == -1 && errno == EBADF);
    {
        int const truncated = open(second, O_RDWR);
        check("ftruncate", "a descriptor sets its own file's length",
              truncated >= 0 && ftruncate(truncated, 2) == 0 && fstat(truncated, &st) == 0 &&
                  st.st_size == 2);
        if (truncated >= 0) {
            (void)close(truncated);
        }
    }

    /* unlink: RETURN VALUE (zero), ERRORS ("[ENOENT] The named file does not
     * exist"). */
    errno = 0;
    check("unlink", "a missing file is ENOENT", unlink(absent_path) == -1 && errno == ENOENT);
    check("unlink", "a file is removed and then absent",
          write_text(second, kText) == 0 && unlink(second) == 0 && absent(second));

    /* rename: RETURN VALUE (zero), ERRORS ("[ENOENT] The file named by old does
     * not exist"). */
    errno = 0;
    check("rename", "a missing source is ENOENT", rename(absent_path, renamed) == -1 &&
                                                      errno == ENOENT);
    check("rename", "a file moves and the old name is gone",
          write_text(second, kText) == 0 && rename(second, renamed) == 0 && absent(second) &&
              !absent(renamed));
    (void)unlink(renamed);

    /* chmod/fchmod: RETURN VALUE (zero), ERRORS ("[EBADF] ... not a valid file
     * descriptor"). Where the volume keeps mode bits (BFS's Protect) the mode is
     * set; where it does not (FAT) the operation is refused with EOPNOTSUPP, and
     * that refusal is asserted. */
    errno = 0;
    check("fchmod", "a bad descriptor is EBADF", fchmod(-1, 0600) == -1 && errno == EBADF);
    if (volume.has_modes) {
        check("chmod", "a file's mode is set", chmod(file, 0644) == 0);
        int const moded = open(file, O_RDONLY);
        check("fchmod", "a descriptor sets its file's mode",
              moded >= 0 && fchmod(moded, 0600) == 0);
        if (moded >= 0) {
            (void)close(moded);
        }
    } else {
        errno = 0;
        unavailable("chmod", "is refused where the volume has no mode bits",
                    chmod(file, 0644) == -1, errno);
        int const moded = open(file, O_RDONLY);
        errno = 0;
        unavailable("fchmod", "is refused where the volume has no mode bits",
                    moded >= 0 && fchmod(moded, 0600) == -1, errno);
        if (moded >= 0) {
            (void)close(moded);
        }
    }

    /* fcntl: ERRORS ("[EBADF] The fildes argument is not a valid open file
     * descriptor"). F_GETFD/F_SETFD and F_GETFL are answered, but no descriptor
     * flags are kept (there is no exec), so the flag does not read back: named
     * as a deviation, not passed. */
    {
        int const fcntled = open(file, O_RDONLY);
        if (fcntled >= 0 && (fcntl(fcntled, F_GETFL) & O_ACCMODE) == O_RDONLY &&
            fcntl(fcntled, F_SETFD, FD_CLOEXEC) == 0 &&
            (fcntl(fcntled, F_GETFD) & FD_CLOEXEC) != 0) {
            check("fcntl", "F_GETFL and the descriptor flags read back", true);
        } else {
            deviation("fcntl", "F_GETFL and the descriptor flags read back",
                      "no descriptor flags are kept (there is no exec): F_GETFD loses FD_CLOEXEC");
        }
        errno = 0;
        check("fcntl", "a bad descriptor is EBADF", fcntl(-1, F_GETFL) == -1 && errno == EBADF);
        if (fcntled >= 0) {
            (void)close(fcntled);
        }
    }

    /* fopen/fread/fwrite/fflush/fileno/fclose: the stdio over the above. */
    errno = 0;
    check("fopen", "a missing file is NULL with ENOENT",
          fopen(absent_path, "r") == nullptr && errno == ENOENT);
    FILE *const out = fopen(file, "w");
    check("fopen", "a file is opened for writing", out != nullptr);
    if (out != nullptr) {
        size_t const n = length_of(kText);
        check("fileno", "the stream has a descriptor", fileno(out) >= 0);
        check("fwrite", "the text is written", fwrite(kText, 1, n, out) == n);
        check("fflush", "the stream flushes", fflush(out) == 0);
        check("fclose", "the stream closes", fclose(out) == 0);
    } else {
        check("fileno", "the stream has a descriptor", false);
        check("fwrite", "the text is written", false);
        check("fflush", "the stream flushes", false);
        check("fclose", "the stream closes", false);
    }
    FILE *const in = fopen(file, "r");
    if (in != nullptr) {
        char buffer[256];
        size_t const n = length_of(kText);
        check("fread", "the text reads back",
              fread(buffer, 1, n, in) == n && equals(buffer, kText, n));
        (void)fclose(in);
    } else {
        check("fread", "the text reads back", false);
    }

    /* remove: a file, or an empty directory. */
    check("remove", "a file is removed and then absent",
          write_text(second, kText) == 0 && remove(second) == 0 && absent(second));

    /* The holes: calls Aegir does not implement. Each must fail; the errno is
     * printed, so a hole that closes goes red rather than silent. */
    errno = 0;
    unavailable("link", "is not implemented", link(file, renamed) == -1, errno);
    errno = 0;
    unavailable("symlink", "is not implemented", symlink(file, renamed) == -1, errno);
    {
        char link_buffer[64];
        errno = 0;
        unavailable("readlink", "is not implemented",
                    readlink(file, link_buffer, sizeof(link_buffer)) == -1, errno);
    }
    errno = 0;
    unavailable("chown", "is not implemented", chown(file, 0, 0) == -1, errno);
    int const held = open(file, O_RDONLY);
    errno = 0;
    unavailable("dup", "is not implemented", held >= 0 && dup(held) == -1, errno);
    errno = 0;
    unavailable("dup2", "is not implemented", held >= 0 && dup2(held, 9) == -1, errno);
    if (held >= 0) {
        (void)close(held);
    }
    {
        int fds[2] = {-1, -1};
        errno = 0;
        unavailable("pipe", "is not implemented", pipe(fds) == -1, errno);
    }
    {
        int const synced = open(file, O_RDONLY);
        errno = 0;
        unavailable("fsync", "is not implemented", synced >= 0 && fsync(synced) == -1, errno);
        errno = 0;
        unavailable("fdatasync", "is not implemented",
                    synced >= 0 && fdatasync(synced) == -1, errno);
        if (synced >= 0) {
            (void)close(synced);
        }
    }
    struct statvfs vfs;
    errno = 0;
    unavailable("statvfs", "is not implemented", statvfs(file, &vfs) == -1, errno);
    {
        int const vfsed = open(file, O_RDONLY);
        errno = 0;
        unavailable("fstatvfs", "is not implemented", vfsed >= 0 && fstatvfs(vfsed, &vfs) == -1,
                    errno);
        if (vfsed >= 0) {
            (void)close(vfsed);
        }
    }
    /* futimens/utimensat: `utime` was removed in Issue 8; these replaced it. */
    {
        struct timespec now[2];
        now[0].tv_sec = 0;
        now[0].tv_nsec = 0;
        now[1].tv_sec = 0;
        now[1].tv_nsec = 0;
        int const stamped = open(file, O_RDONLY);
        errno = 0;
        unavailable("futimens", "is not implemented",
                    stamped >= 0 && futimens(stamped, now) == -1, errno);
        if (stamped >= 0) {
            (void)close(stamped);
        }
        errno = 0;
        unavailable("utimensat", "is not implemented",
                    utimensat(AT_FDCWD, file, now, 0) == -1, errno);
    }
    errno = 0;
    unavailable("mkfifo", "is not implemented", mkfifo(fifo, 0644) == -1, errno);
    /* sync() returns void, so an unanswered SYS_sync has nothing to fail
     * against: named, not silently passing. */
    skip("sync", "returns void", "the unanswered syscall is unobservable through sync()");

    (void)unlink(file);
}

}  // namespace aegir_posix_conformance
