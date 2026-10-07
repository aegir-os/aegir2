/*
 * aegir-posix-env-test: the environment-and-time sub-arc's client (specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A plain program in the POSIX sense: getenv, setenv, unsetenv, environ, uname,
 * sysconf, getcwd, chdir, clock_gettime, nanosleep and printf -- libc calls, no
 * Aegir call of its own. The runtime stands it up before `main` (aegir-crt0's
 * constructor), and its evidence is the marker it prints, which the terminal
 * mirrors to the serial the acceptance's runner reads.
 *
 * What it proves is the *C mapping* of a decision the native side already made
 * (specs/environment.md): the environment a process was spawned with, the
 * environment it changes for itself, what `uname` says the system is, what
 * `sysconf` answers about it, and that the clock it sleeps against is the one it
 * reads. `specs/clang-on-aegir.md:98` is why this is the sub-arc: a compiler
 * probes `uname` and `sysconf` and reads `PATH`/`TMPDIR` out of its environment,
 * and the first two had no answer at all.
 *
 * What it *inherited* is printed rather than asserted: the session's environment
 * is the running system's business, and a client that pinned a value would fail
 * for a reason that is not the layer's (specs/posix.md).
 *
 * Statuses:
 *    0  every check passed (and it says AEGIR_POSIX_ENV_OK on stdout as well)
 *  2+n  the n-th check failed (n counts the calls to require(), in source order)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

namespace {

constexpr char const *kName = "AEGIR_POSIX_ENV";
constexpr char const *kValue = "yes";

int check = 0;

[[noreturn]] void fail(int status, char const *what)
{
    printf("AEGIR_POSIX_ENV_FAIL %s (check %d)\n", what, status - 2);
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

/* Whether `NAME=VALUE` is one of the strings the process's environment holds. */
bool environ_has(char const *entry)
{
    for (char **it = environ; it != nullptr && *it != nullptr; ++it) {
        if (strcmp(*it, entry) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char *argv[])
{
    /* What the spawner gave, through the startup frame the runtime passes on
     * (specs/environment.md: arguments are a list of strings, and argv[0] is the
     * process's own name). */
    require(argc >= 1 && argv != nullptr && argv[0] != nullptr && argv[0][0] != '\0',
            "argc/argv come from the spawner");

    /* The environment a process changes for itself: set, read back, see it in
     * `environ`, then take it away again. */
    require(setenv(kName, kValue, 1) == 0, "setenv");
    char const *const got = getenv(kName);
    require(got != nullptr && strcmp(got, kValue) == 0, "getenv reads what setenv set");
    require(environ_has("AEGIR_POSIX_ENV=yes"), "environ lists what setenv set");
    require(unsetenv(kName) == 0, "unsetenv");
    require(getenv(kName) == nullptr, "getenv is NULL after unsetenv");

    /* What the system says it is. A compiler reads this to pick a target and a
     * host: sysname is the OS, machine the architecture (specs/posix.md's
     * decision for uname's answers, and specs/clang-on-aegir.md:98). */
    struct utsname info {};
    require(uname(&info) == 0, "uname");
    require(strcmp(info.sysname, "Aegir") == 0, "uname sysname is Aegir");
    require(strcmp(info.machine, "riscv64") == 0, "uname machine is riscv64");
    require(info.release[0] != '\0' && info.version[0] != '\0',
            "uname carries a release and a version");

    /* What sysconf answers about it: the page size a program sizes its buffers
     * with, the clock tick, and how many processors there are. No invented
     * ceilings -- a limit here would be one (AGENTS.md). */
    require(sysconf(_SC_PAGESIZE) == 4096, "sysconf _SC_PAGESIZE");
    require(sysconf(_SC_CLK_TCK) >= 1, "sysconf _SC_CLK_TCK");
    require(sysconf(_SC_NPROCESSORS_ONLN) >= 1, "sysconf _SC_NPROCESSORS_ONLN");

    /* The clock it sleeps against is the one it reads: a monotonic clock that
     * moves across a sleep (aegir-clock behind the dispatcher's cases). */
    struct timespec before {};
    struct timespec after {};
    require(clock_gettime(CLOCK_MONOTONIC, &before) == 0, "clock_gettime");
    struct timespec nap {};
    nap.tv_nsec = 2 * 1000 * 1000;
    require(nanosleep(&nap, nullptr) == 0, "nanosleep");
    require(clock_gettime(CLOCK_MONOTONIC, &after) == 0, "clock_gettime after the sleep");
    bool const moved = after.tv_sec > before.tv_sec ||
                       (after.tv_sec == before.tv_sec && after.tv_nsec > before.tv_nsec);
    require(moved, "the clock moved across the sleep");

    /* The current directory, through the path view (specs/posix.md): this client
     * runs with the others, so it re-asserts the one string the view and the
     * native side both read. */
    char cwd[256];
    require(chdir("/Sys") == 0, "chdir /Sys");
    require(getcwd(cwd, sizeof(cwd)) != nullptr && strcmp(cwd, "/Sys") == 0,
            "getcwd is /Sys");

    /* Printed, not asserted: what this process was spawned with. */
    printf("AEGIR_POSIX_ENV inherited:\n");
    for (char **it = environ; it != nullptr && *it != nullptr; ++it) {
        printf("  %s\n", *it);
    }

    printf("AEGIR_POSIX_ENV_OK\n");
    (void)fflush(nullptr);
    _exit(0);
}
