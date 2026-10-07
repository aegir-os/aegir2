/*
 * aegir-posix-file-test: the file sub-arc's acceptance client (specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A plain program in the POSIX sense: creat, write, read, lseek, fstat,
 * ftruncate/truncate, rename, unlink, mkdir, rmdir, opendir and readdir, and
 * nothing else -- so it carries no Aegir call of its own, and the runtime stands
 * it up before `main` (aegir-crt0's constructor). Its evidence is a marker it
 * prints itself: `printf` is a libc call, not the runtime's diagnostic writer, so
 * saying AEGIR_POSIX_FILE_OK is still something a program that does not know it
 * is on Aegir can do, and the acceptance's step cues on that line.
 *
 * What it proves is the writing half of the file surface, on the volume the
 * tree already treats as its scratchpad (`SCRATCH:`, and `/SCRATCH` through the
 * path view): a file created through the view with a directory component in its
 * path, written in one call, seeked, read back byte for byte, sized, truncated
 * through the descriptor and then by name, renamed, listed, and removed; a
 * directory made, filled and removed; and a name that is gone staying gone.
 *
 * Statuses:
 *    0  every check passed (and it says AEGIR_POSIX_FILE_OK on stdout as well)
 *  2+n  the n-th check failed (n counts the calls to require(), in source
 *       order, from the first one to run)
 *
 * The status is per check on purpose. A hosted command's stdout does not reach
 * the console yet (nothing installs fds 0/1/2: libs/aegir-posix/src/files.cc's
 * install() is called only from openat, so write(1, ...) is EBADF), which means
 * this program's own marker and its failure line are both dropped. What does
 * reach the acceptance is the terminal's own `command exited <n>` line, so the
 * status carries the name the message cannot: 2+n says which check, counted in
 * source order.
 *
 * Success is 0 and the marker is the cue, which is the shape the process
 * sub-arc's client already has (AEGIR_POSIX_WAIT_OK). A non-zero success status
 * would not survive this session: the shell's Shell-Startup is a script and a
 * script stops at the first command that exits non-zero. Measured, on the run
 * that put this client one line behind the path view's: the path client exited
 * 63 and this one, staged and on the disk the whole time, never started.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

/* A directory component in the path on purpose: a created file's path goes
 * through the same translation as a read one, and a two-component path is what
 * exercises the join rather than a bare name at the volume's root. */
constexpr char const *kDirectory = "/SCRATCH/POSIX.DIR";
constexpr char const *kFile = "/SCRATCH/POSIX.DIR/FILE.TXT";
constexpr char const *kRenamed = "/SCRATCH/POSIX.DIR/RENAMED.TXT";

/* Longer than a volume block, so the write crosses the block boundary the
 * volume's own write has to handle rather than fitting in one short hold. */
char const *const kText =
    "the file sub-arc's client wrote this through the POSIX view, past a volume "
    "block boundary, and read it back whole\n";

/* Which check is running, so a failure can say so with a status rather than a
 * message that is dropped (the header). */
int check = 0;

[[noreturn]] void fail(int status, char const *what)
{
    printf("AEGIR_POSIX_FILE_FAIL %s (errno %d, check %d)\n", what, errno, status - 2);
    (void)fflush(nullptr);
    _exit(status);
}

void require(bool ok, char const *what)
{
    ++check;
    if (!ok) {
        fail(2 + check, what);
    }
}

/* Whether `path` reads back whole as `want`. */
bool reads_text(char const *path, char const *want)
{
    int const fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char buffer[256];
    ssize_t const got = read(fd, buffer, sizeof(buffer));
    (void)close(fd);
    size_t const length = strlen(want);
    return got == static_cast<ssize_t>(length) && memcmp(buffer, want, length) == 0;
}

/* Whether `path` names nothing at all: a name that was removed has to read as
 * absent, not as a handle that outlived it. */
bool absent(char const *path)
{
    int const fd = open(path, O_RDONLY);
    if (fd >= 0) {
        (void)close(fd);
        return false;
    }
    return errno == ENOENT;
}

/* Whether `directory` lists `name`. */
bool lists(char const *directory, char const *name)
{
    DIR *const dir = opendir(directory);
    if (dir == nullptr) {
        fail(2 + ++check, "opendir");
    }
    bool found = false;
    while (struct dirent *entry = readdir(dir)) {
        if (strcmp(entry->d_name, name) == 0) {
            found = true;
            break;
        }
    }
    (void)closedir(dir);
    return found;
}

}  // namespace

int main()
{
    struct stat info {};
    size_t const length = strlen(kText);

    /* The scratchpad starts empty: the disk is built for each run and the run's
     * writes land in a throwaway overlay (scripts/run_target.py), so every name
     * this program creates is its own. */
    require(absent(kFile), "the scratch file starts absent");
    require(mkdir(kDirectory, 0755) == 0, "mkdir /SCRATCH/POSIX.DIR");
    require(lists("/SCRATCH", "POSIX.DIR"), "/SCRATCH lists POSIX.DIR");
    require(stat(kDirectory, &info) == 0 && S_ISDIR(info.st_mode),
            "stat /SCRATCH/POSIX.DIR is a directory");

    /* Create through the view, write in one call, and read it back whole. */
    int fd = open(kFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    require(fd >= 0, "create /SCRATCH/POSIX.DIR/FILE.TXT");
    require(write(fd, kText, length) == static_cast<ssize_t>(length), "write the file");
    require(lseek(fd, 0, SEEK_CUR) == static_cast<long>(length),
            "lseek SEEK_CUR is where the write left off");
    require(lseek(fd, 0, SEEK_SET) == 0, "lseek SEEK_SET is the start");
    require(fstat(fd, &info) == 0 && info.st_size == static_cast<off_t>(length),
            "fstat size is what was written");
    require(close(fd) == 0, "close after writing");
    require(reads_text(kFile, kText), "read the file back whole");

    /* Truncate through the descriptor, then by name: both spellings reach the
     * volume's own truncate, and the file is the shorter thing afterwards. */
    fd = open(kFile, O_RDWR);
    require(fd >= 0, "reopen read/write");
    require(ftruncate(fd, static_cast<off_t>(length)) == 0, "ftruncate to its own length");
    require(fstat(fd, &info) == 0 && info.st_size == static_cast<off_t>(length),
            "fstat after ftruncate");
    require(close(fd) == 0, "close after ftruncate");
    require(truncate(kFile, 4) == 0, "truncate by name to 4 bytes");
    require(stat(kFile, &info) == 0 && info.st_size == 4, "stat says 4 bytes");
    {
        char head[8] = {};
        fd = open(kFile, O_RDONLY);
        require(fd >= 0, "reopen after the truncate");
        require(read(fd, head, sizeof(head)) == 4, "a truncated file reads 4 bytes");
        require(memcmp(head, kText, 4) == 0, "and they are the text's first bytes");
        require(close(fd) == 0, "close after reading the truncation");
    }

    /* Rename within the volume: the old name goes, the new one arrives with the
     * bytes the truncate left. */
    require(rename(kFile, kRenamed) == 0, "rename FILE.TXT to RENAMED.TXT");
    require(absent(kFile), "the renamed-away name is gone");
    require(lists(kDirectory, "RENAMED.TXT"), "the directory lists RENAMED.TXT");
    {
        char head[8] = {};
        fd = open(kRenamed, O_RDONLY);
        require(fd >= 0, "open the renamed file");
        require(read(fd, head, sizeof(head)) == 4, "the renamed file still holds 4 bytes");
        require(memcmp(head, kText, 4) == 0, "and they are the text's first bytes");
        require(close(fd) == 0, "close the renamed file");
    }

    /* Remove: the name goes, the directory that held it goes, and a read of
     * either fails with ENOENT rather than a handle that outlived it. */
    require(unlink(kRenamed) == 0, "unlink RENAMED.TXT");
    require(absent(kRenamed), "the unlinked name is gone");
    require(rmdir(kDirectory) == 0, "rmdir /SCRATCH/POSIX.DIR");
    require(absent(kDirectory), "the removed directory is gone");

    printf("AEGIR_POSIX_FILE_OK\n");
    (void)fflush(nullptr);
    _exit(0);
}
