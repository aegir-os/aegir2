/*
 * aegir-posix-path-test: the path view's acceptance client (specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A plain program in the POSIX sense: every call it makes is a libc call --
 * open, read, close, chdir, getcwd, stat, opendir and readdir -- so it carries
 * no Aegir call of its own. The runtime stands it up before `main` (aegir-crt0's
 * constructor), and its evidence is a marker it prints itself: `printf` is a
 * libc call, not the runtime's diagnostic writer, so saying AEGIR_POSIX_PATH_OK
 * is still something a program that does not know it is on Aegir can do, and the
 * acceptance's step cues on that line.
 *
 * What it proves is the `/`-rooted view: a volume under `/`, `.` and `..`
 * normalized against the composed path, the first component folded by the
 * namespace while the name below it is kept as written, the `Sys:` alias
 * reached through the view, the current directory a POSIX path, and `/` itself
 * a directory whose entries are the namespace's volumes and bindings.
 *
 * Statuses:
 *    0  every check passed (and it says AEGIR_POSIX_PATH_OK on stdout as well)
 *    1  a check failed, named on stdout -- the terminal's grid, not the serial
 *
 * Success is 0 and the marker is the cue, which is the shape the process
 * sub-arc's client already has (AEGIR_POSIX_WAIT_OK). A non-zero success status
 * does not survive this session: the shell's Shell-Startup is a script and a
 * script stops at the first command that exits non-zero. This client exited 63
 * while the view was the last line of that script, and the run that added the
 * file sub-arc's client behind it is the measurement: that client was staged
 * under Sys:Development/C and on the disk, and it never started at all. Failure
 * still exits non-zero, so a failed check leaves the marker unprinted and the run
 * fails naming it (scripts/targets.py and AGENTS.md's trigger rule: a cue has to
 * be unique and has to be the proof).
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

    /* A *file* a directory deep inside a volume, and one at its root: `stat` must
     * walk as far as `open` does. This is the call lld's existence check makes,
     * and it is the one that failed on the staged sysroot while `open` succeeded
     * beside it (specs/clang-on-aegir.md's Phase 3). */
    require(stat("/AEGIR/DOCS/NESTED.TXT", &info) == 0 && S_ISREG(info.st_mode),
            "stat /AEGIR/DOCS/NESTED.TXT is a regular file");
    require(stat("/AEGIR/AEGIR.TXT", &info) == 0 && S_ISREG(info.st_mode),
            "stat /AEGIR/AEGIR.TXT is a regular file");

    /* The *same* volume by the route lld's existence check takes: invoked with a
     * `Sys:` path, this is a stat through the binding (`Sys:` aliases the system
     * volume), not through the volume's own name. Both forms are native, so both
     * skip the view's normalization (specs/clang-on-aegir.md's Phase 3). */
    require(stat("Sys:AEGIR.TXT", &info) == 0 && S_ISREG(info.st_mode),
            "stat Sys:AEGIR.TXT is a regular file");
    require(stat("Sys:DOCS/NESTED.TXT", &info) == 0 && S_ISREG(info.st_mode),
            "stat Sys:DOCS/NESTED.TXT is a regular file");

    /* Three components inside the volume, by both calls and both routes: the depth
     * the staged sysroot sits at, and the one where `stat` and `open` were last
     * seen disagreeing (specs/clang-on-aegir.md's Phase 3). */
    require(reads_text("/AEGIR/DOCS/DEEPER/INNER.TXT",
                       "three components deep, and the walk ends there\n"),
            "open /AEGIR/DOCS/DEEPER/INNER.TXT");
    require(stat("/AEGIR/DOCS/DEEPER/INNER.TXT", &info) == 0 && S_ISREG(info.st_mode),
            "stat /AEGIR/DOCS/DEEPER/INNER.TXT is a regular file");
    require(stat("Sys:DOCS/DEEPER/INNER.TXT", &info) == 0 && S_ISREG(info.st_mode),
            "stat Sys:DOCS/DEEPER/INNER.TXT is a regular file");

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
    _exit(0);
}
