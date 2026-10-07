/*
 * aegir-posix-path-test: the path view's acceptance client (specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A plain program in the POSIX sense: every call it makes is a libc call --
 * open, read, close, chdir, getcwd, stat, opendir and readdir -- so it carries
 * no Aegir call of its own. The runtime stands it up before `main` (aegir-crt0's
 * constructor), and its evidence is its own exit status, because a program that
 * must not know it is on Aegir cannot call the runtime's diagnostic writer to
 * say so. That is the shape `aegir-echo`'s code already has in the DOS
 * acceptance: the terminal's own `command exited <n>` line is the cue.
 *
 * What it proves is the `/`-rooted view: a volume under `/`, `.` and `..`
 * normalized against the composed path, the first component folded by the
 * namespace while the name below it is kept as written, the `Sys:` alias
 * reached through the view, the current directory a POSIX path, and `/` itself
 * a directory whose entries are the namespace's volumes and bindings.
 *
 * Statuses:
 *   63  every check passed (and it says AEGIR_POSIX_PATH_OK on stdout as well)
 *    1  a check failed, named on stdout -- the terminal's grid, not the serial
 *
 * 63 is unique among the codes a dev target's session produces -- 0, the DOS
 * acceptance's 10, 42 and 77, the console's break at 20 -- so the acceptance's
 * QmpStep cues on `terminal: command exited 63` exactly, and a failure leaves
 * that cue unprinted, which is how the run fails (scripts/targets.py and
 * AGENTS.md's trigger rule: a cue has to be unique and has to be the proof).
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

/* What the AEGIR volume holds, byte for byte (scripts/make_disk.py's
 * AEGIR_BFS_TREE): a file at the volume's root, and one a directory down --
 * the second is what a relative path and a component walk are measured with. */
char const *const kAegirText = "aegir read this file off a disk it enumerated itself\n";
char const *const kNestedText = "two components deep, and the walk found it\n";

constexpr int kOkStatus = 63;

[[noreturn]] void fail(char const *what)
{
    printf("AEGIR_POSIX_PATH_FAIL %s\n", what);
    (void)fflush(nullptr);
    _exit(1);
}

void require(bool ok, char const *what)
{
    if (!ok) {
        fail(what);
    }
}

/* Whether `path` reads back whole as `want`: the view's translation is right
 * when the bytes are, and nothing else can say that. */
bool reads_text(char const *path, char const *want)
{
    int const fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char buffer[128];
    ssize_t const got = read(fd, buffer, sizeof(buffer));
    (void)close(fd);
    size_t const length = strlen(want);
    return got == static_cast<ssize_t>(length) && memcmp(buffer, want, length) == 0;
}

bool reads_aegir_text(char const *path)
{
    return reads_text(path, kAegirText);
}

/* Whether `/` lists `name`. Its entries are the namespace's own: every volume
 * and every binding, one each (specs/posix.md). */
bool lists(char const *name)
{
    DIR *const dir = opendir("/");
    if (dir == nullptr) {
        fail("opendir /");
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
    char cwd[256];

    /* `/` is a directory: the synthetic root, which no volume names. */
    require(stat("/", &info) == 0 && S_ISDIR(info.st_mode), "stat / is a directory");

    /* A volume's file through the view: `/Name/rest` is `Name:rest`. */
    require(reads_aegir_text("/AEGIR/AEGIR.TXT"), "open /AEGIR/AEGIR.TXT");

    /* `.` and `..` are normalized against the composed path, because the
     * Amiga's directories have none, and the first component is folded by the
     * namespace: `/aegir` and `/AEGIR` are one volume, while the name below it
     * is kept as written (the volume is BFS, which does not fold). */
    require(reads_aegir_text("/./AEGIR/AEGIR.TXT"), "open /./AEGIR/AEGIR.TXT");
    require(reads_aegir_text("/AEGIR/DOCS/../AEGIR.TXT"), "open /AEGIR/DOCS/../AEGIR.TXT");
    require(reads_aegir_text("/aegir/AEGIR.TXT"),
            "open /aegir/AEGIR.TXT (the first component is folded)");

    /* The alias half of the table: `Sys:` names the same volume, so /Sys is the
     * same place as /AEGIR. */
    require(reads_aegir_text("/Sys/AEGIR.TXT"), "open /Sys/AEGIR.TXT");

    /* A volume's root is a directory to the view. */
    require(stat("/AEGIR", &info) == 0 && S_ISDIR(info.st_mode),
            "stat /AEGIR is a directory");

    /* A native path sets the directory in Aegir's own grammar, and getcwd answers
     * it that way: libc++'s std::filesystem::absolute and LLVM's
     * sys::fs::make_absolute compose with this string and expect a `Volume:`
     * root, and both run in processes that also speak the view
     * (specs/posix.md, specs/environment.md). It is asserted here because one
     * string serves both readers, and the view's form is the *other* answer. */
    require(chdir("Sys:") == 0, "chdir Sys:");
    require(getcwd(cwd, sizeof(cwd)) != nullptr && strcmp(cwd, "Sys:") == 0,
            "getcwd after a native chdir is the Aegir grammar");

    /* The current directory is a POSIX path the layer maps: chdir of /Sys is
     * the VFS's Sys:, and getcwd answers /Sys back. */
    require(chdir("/Sys") == 0, "chdir /Sys");
    require(getcwd(cwd, sizeof(cwd)) != nullptr && strcmp(cwd, "/Sys") == 0,
            "getcwd is /Sys");
    require(chdir("/AEGIR/DOCS") == 0, "chdir /AEGIR/DOCS");
    require(getcwd(cwd, sizeof(cwd)) != nullptr && strcmp(cwd, "/AEGIR/DOCS") == 0,
            "getcwd is /AEGIR/DOCS");

    /* A relative path joins it, and `..` in that composition is resolved here
     * (the Amiga's directories have no `..` of their own). */
    require(reads_text("NESTED.TXT", kNestedText), "open NESTED.TXT in /AEGIR/DOCS");
    require(reads_aegir_text("../AEGIR.TXT"), "open ../AEGIR.TXT");

    /* `..` above a volume's root is `/`, and getcwd says so. */
    require(chdir("/AEGIR/..") == 0, "chdir /AEGIR/..");
    require(getcwd(cwd, sizeof(cwd)) != nullptr && strcmp(cwd, "/") == 0,
            "getcwd is / after ..");

    /* `/` lists the namespace: every volume and every binding, one each, and a
     * volume that says it has no directory (NIL:) is listed all the same. */
    require(lists("AEGIR"), "/ lists AEGIR");
    require(lists("Sys"), "/ lists Sys");
    require(lists("NIL"), "/ lists NIL");

    printf("AEGIR_POSIX_PATH_OK\n");
    (void)fflush(nullptr);
    _exit(kOkStatus);
}
