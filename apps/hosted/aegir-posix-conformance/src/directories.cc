/*
 * aegir-posix-conformance: the directories area.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Every case is derived from the page named beside it in
 * third_party/posix-spec/issue8 (fetched by `make deps`): RETURN VALUE and
 * ERRORS are the source, and the clause is quoted where a case is not
 * self-evident. The area runs against one volume, and the caller runs it
 * against a FAT and a BFS one (scratch.h), because mkdir/rmdir and the name a
 * listing returns are properties of the filesystem.
 */

#include "harness.h"
#include "scratch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aegir_posix_conformance {
namespace {

void under(Volume const &volume, char const *name, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", volume.root, name);
}

/* Whether an entry's name is exactly `name`. The fixture names are uppercase, as
 * a FAT directory stores them, so both volumes answer the same thing. */
bool named(struct dirent const *entry, char const *name)
{
    size_t const n = length_of(name);
    return equals(entry->d_name, name, n) && entry->d_name[n] == '\0';
}

void copy_name(char *out, size_t size, char const *name)
{
    size_t i = 0;
    for (; name[i] != '\0' && i + 1 < size; ++i) {
        out[i] = name[i];
    }
    out[i] = '\0';
}

/* A case whose outcome may be a documented error instead of a pass. */
void conforms_or_deviation(char const *interface, char const *test, char const *clause,
                           bool conforms, int observed_errno, char const *expected)
{
    if (conforms) {
        check(interface, test, true);
    } else {
        char reason[128];
        snprintf(reason, sizeof(reason), "%s names %s; Aegir answers errno %d", clause, expected,
                 observed_errno);
        deviation(interface, test, reason);
    }
}

}  // namespace

void directories_surface(Volume const &volume)
{
    char title[40];
    snprintf(title, sizeof(title), "directories (%s)", volume.label);
    section(title);

    if (!wait_for_volume(volume)) {
        check("opendir", "the volume is available", false);
        return;
    }

    /* chdir/getcwd are process-wide, so what this area sets is put back. */
    char saved_cwd[256];
    bool const have_saved = getcwd(saved_cwd, sizeof(saved_cwd)) != nullptr;

    char dir[160];
    char sub[160];
    char inside[160];
    char missing[160];
    char deep[160];
    under(volume, "POSIX.DIR", dir, sizeof(dir));
    under(volume, "POSIX.DIR/SUB", sub, sizeof(sub));
    under(volume, "POSIX.DIR/INSIDE.TXT", inside, sizeof(inside));
    under(volume, "POSIX.DIR/NOPE", missing, sizeof(missing));
    under(volume, "POSIX.DIR/NOPE/DEEP", deep, sizeof(deep));

    /* mkdir.html ERRORS: "[EEXIST] The named file exists"; "[ENOENT] A component
     * of the path prefix of path does not name an existing file." */
    (void)mkdir(dir, 0755);
    errno = 0;
    check("mkdir", "an existing name is EEXIST", mkdir(dir, 0755) == -1 && errno == EEXIST);
    errno = 0;
    int const deep_result = mkdir(deep, 0755);
    int const deep_errno = errno;
    conforms_or_deviation("mkdir", "a missing path component is ENOENT", "mkdir.html",
                          deep_result == -1 && deep_errno == ENOENT, deep_errno, "ENOENT");
    if (deep_result == 0) {
        /* FAT's answer_mkdir runs `make_dirs`, so it made the missing component
         * too. Remove what it made, so the fixture is what the area intended and
         * the chdir/rmdir cases below are not reading the leavings. */
        (void)rmdir(deep);
        (void)rmdir(missing);
    }
    check("mkdir", "a fresh directory is created", mkdir(sub, 0755) == 0);

    /* A file inside, so a listing has something to find. */
    {
        int const fd = open(inside, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd >= 0) {
            (void)close(fd);
        }
    }

    /* opendir.html RETURN VALUE and ERRORS. */
    DIR *const stream = opendir(dir);
    check("opendir", "an existing directory opens", stream != nullptr);
    if (stream != nullptr) {
        /* readdir.html RETURN VALUE: an entry, or null at the end. */
        char first[256];
        first[0] = '\0';
        bool found_sub = false;
        bool found_inside = false;
        struct dirent *entry = nullptr;
        while ((entry = readdir(stream)) != nullptr) {
            if (first[0] == '\0') {
                copy_name(first, sizeof(first), entry->d_name);
            }
            if (named(entry, "SUB")) {
                found_sub = true;
            }
            if (named(entry, "INSIDE.TXT")) {
                found_inside = true;
            }
        }
        check("readdir", "the listing names the directory and the file",
              found_sub && found_inside);
        check("readdir", "the end of the directory is a null pointer", entry == nullptr);

        /* dirfd.html RETURN VALUE: "an integer which contains a file descriptor
         * for the stream". */
        int const fd = dirfd(stream);
        check("dirfd", "the stream has a descriptor", fd >= 0);

        /* rewinddir.html: "shall not return a value"; the stream is back at the
         * beginning, so the first entry comes again. musl's rewinddir is an
         * lseek on the stream's descriptor, so lseek.html's rule that a
         * directory's offset is seekable is what it rests on. */
        errno = 0;
        int const dir_seek = lseek(dirfd(stream), 0, SEEK_SET);
        int const dir_seek_errno = errno;
        conforms_or_deviation("lseek", "a directory descriptor seeks", "lseek.html",
                              dir_seek == 0, dir_seek_errno, "a zero offset");
        rewinddir(stream);
        struct dirent *const first_again = readdir(stream);
        int const rewind_errno = errno;
        conforms_or_deviation("rewinddir", "the stream starts again at the first entry",
                              "rewinddir.html",
                              first_again != nullptr && first[0] != '\0' && named(first_again, first),
                              rewind_errno, "the first entry again");

        /* telldir.html RETURN VALUE: the current location; seekdir.html: "shall
         * not return a value", and a position seeks back to its entry. */
        long const at = telldir(stream);
        struct dirent *const after = readdir(stream);
        char after_name[256];
        after_name[0] = '\0';
        if (after != nullptr) {
            copy_name(after_name, sizeof(after_name), after->d_name);
        }
        seekdir(stream, at);
        struct dirent *const again = readdir(stream);
        int const seek_errno = errno;
        conforms_or_deviation("seekdir", "a position returns to the entry it named",
                              "seekdir.html",
                              after != nullptr && again != nullptr && named(again, after_name),
                              seek_errno, "the entry it named");
        check("telldir", "the current location is a position", at != -1);

        /* closedir.html RETURN VALUE: zero. */
        check("closedir", "the stream closes", closedir(stream) == 0);
    } else {
        check("readdir", "the listing names the directory and the file", false);
        check("readdir", "the end of the directory is a null pointer", false);
        check("dirfd", "the stream has a descriptor", false);
        check("lseek", "a directory descriptor seeks", false);
        check("rewinddir", "the stream starts again at the first entry", false);
        check("seekdir", "a position returns to the entry it named", false);
        check("telldir", "the current location is a position", false);
        check("closedir", "the stream closes", false);
    }

    /* opendir.html ERRORS: "[ENOENT] A component of dirname does not name an
     * existing directory"; "[ENOTDIR] A component of dirname names an existing
     * file that is neither a directory nor a symbolic link to a directory." */
    errno = 0;
    DIR *const absent_dir = opendir(missing);
    int const absent_dir_errno = errno;
    if (absent_dir != nullptr) {
        (void)closedir(absent_dir);
    }
    conforms_or_deviation("opendir", "a missing directory is ENOENT", "opendir.html",
                          absent_dir == nullptr && absent_dir_errno == ENOENT, absent_dir_errno,
                          "ENOENT");
    errno = 0;
    DIR *const file_dir = opendir(inside);
    int const file_dir_errno = errno;
    if (file_dir != nullptr) {
        (void)closedir(file_dir);
    }
    conforms_or_deviation("opendir", "a file is ENOTDIR", "opendir.html",
                          file_dir == nullptr && file_dir_errno == ENOTDIR, file_dir_errno,
                          "ENOTDIR");

    /* scandir.html RETURN VALUE: the number of entries (a negative value on
     * failure). */
    struct dirent **list = nullptr;
    int const count = scandir(dir, &list, nullptr, alphasort);
    check("scandir", "the directory is scanned", count >= 0);
    if (list != nullptr) {
        for (int i = 0; i < count; ++i) {
            free(list[i]);
        }
        free(list);
    }
    errno = 0;
    struct dirent **none = nullptr;
    int const missing_count = scandir(missing, &none, nullptr, alphasort);
    int const missing_errno = errno;
    if (none != nullptr) {
        free(none);
    }
    conforms_or_deviation("scandir", "a missing directory is ENOENT", "scandir.html",
                          missing_count < 0 && missing_errno == ENOENT, missing_errno, "ENOENT");

    /* chdir.html ERRORS: "[ENOENT] A component of path does not name an existing
     * directory"; "[ENOTDIR] A component of the pathname names an existing file
     * that is neither a directory nor a symbolic link to a directory." */
    errno = 0;
    int const missing_chdir = chdir(missing);
    int const missing_chdir_errno = errno;
    conforms_or_deviation("chdir", "a missing directory is ENOENT", "chdir.html",
                          missing_chdir == -1 && missing_chdir_errno == ENOENT, missing_chdir_errno,
                          "ENOENT");
    errno = 0;
    int const file_chdir = chdir(inside);
    int const file_chdir_errno = errno;
    conforms_or_deviation("chdir", "a file is ENOTDIR", "chdir.html",
                          file_chdir == -1 && file_chdir_errno == ENOTDIR, file_chdir_errno,
                          "ENOTDIR");

    /* getcwd.html ERRORS: "[EINVAL] The size argument is 0"; "[ERANGE] The size
     * argument is greater than 0, but is smaller than the length of the string
     * +1." */
    char cwd[256];
    errno = 0;
    check("getcwd", "a zero size is EINVAL", getcwd(cwd, 0) == nullptr && errno == EINVAL);
    errno = 0;
    check("getcwd", "a short buffer is ERANGE", getcwd(cwd, 1) == nullptr && errno == ERANGE);
    check("chdir", "an existing directory is entered", chdir(dir) == 0);
    char here[256];
    if (getcwd(here, sizeof(here)) != nullptr) {
        conforms_or_deviation("getcwd", "it answers the directory that was entered", "getcwd.html",
                              equals(here, dir, length_of(dir)) &&
                                  here[length_of(dir)] == '\0',
                              0, dir);
    } else {
        check("getcwd", "it answers the directory that was entered", false);
    }
    if (have_saved) {
        (void)chdir(saved_cwd);
    }

    /* rmdir.html ERRORS: "[EEXIST] or [ENOTEMPTY] The path argument names a
     * directory that is not an empty directory"; "[ENOENT] ... names a
     * nonexistent directory." */
    errno = 0;
    int const not_empty = rmdir(dir);
    int const not_empty_errno = errno;
    conforms_or_deviation("rmdir", "a non-empty directory is ENOTEMPTY", "rmdir.html",
                          not_empty == -1 && (not_empty_errno == ENOTEMPTY ||
                                              not_empty_errno == EEXIST),
                          not_empty_errno, "ENOTEMPTY");
    errno = 0;
    check("rmdir", "a missing directory is ENOENT", rmdir(missing) == -1 && errno == ENOENT);
    (void)unlink(inside);
    check("rmdir", "an empty subdirectory is removed", rmdir(sub) == 0);
    check("rmdir", "an empty directory is removed", rmdir(dir) == 0);

    /* fchdir.html ERRORS: "[EBADF] The fildes argument is not an open file
     * descriptor"; "[ENOTDIR] ... does not refer to a directory." */
    int const chdired = open(volume.root, O_RDONLY | O_DIRECTORY);
    errno = 0;
    unavailable("fchdir", "is not implemented", chdired >= 0 && fchdir(chdired) == -1, errno);
    if (chdired >= 0) {
        (void)close(chdired);
    }
}

}  // namespace aegir_posix_conformance
