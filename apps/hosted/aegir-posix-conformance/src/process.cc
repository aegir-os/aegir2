/*
 * aegir-posix-conformance: the process area.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Every case is derived from the page named beside it in
 * third_party/posix-spec/issue8 (fetched by `make deps`): RETURN VALUE and
 * ERRORS are the source, and the clause is quoted where a case is not
 * self-evident. Termination (exit/_exit/_Exit/abort) does not return, so its
 * case is the process itself and is a named skip; what the area can observe is
 * atexit's registration and every call that either answers a value or fails.
 */

#include "harness.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace aegir_posix_conformance {
namespace {

void noop() {}

/* An interface whose page defines no error case (getpid.html, getuid.html, ...):
 * it either answers a value or it is a hole, and the hole names the errno. */
void answers_or_hole(char const *interface, char const *test, bool answered, int err)
{
    if (answered) {
        check(interface, test, true);
    } else {
        unavailable(interface, test, true, err);
    }
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

void process_surface()
{
    section("process");

    /* exit.html/_exit.html/_Exit.html/abort.html define no error: the call does
     * not return, so its case is the process. abort.html is a raise of SIGABRT,
     * and Aegir delivers no signals (specs/posix.md). atexit.html's registration
     * is observable, and the C++ smoke proves the handler runs. */
    check("atexit", "a handler registers", atexit(noop) == 0);
    skip("exit", "terminates the process", "the process is the unit under test");
    skip("_exit", "terminates the process", "the process is the unit under test");
    skip("_Exit", "terminates the process", "the process is the unit under test");
    skip("abort", "terminates the process", "Aegir delivers no signals");

    /* fork.html ERRORS: EAGAIN, ENOMEM. */
    errno = 0;
    unavailable("fork", "is not implemented", fork() == -1, errno);

    /* exec.html: exec returns only on failure. */
    {
        char *const argv[] = {(char *)"aegir-conformance", nullptr};
        char *const envp[] = {nullptr};
        errno = 0;
        int const result = execve("/SCRATCH/POSIX.NOPE", argv, envp);
        int const result_errno = errno;
        conforms_or_deviation("exec", "a missing program is ENOENT", "exec.html",
                              result == -1 && result_errno == ENOENT, result_errno, "ENOENT");
    }

    /* posix_spawn.html: an error value is returned (not -1/errno), and a missing
     * program is one exec would refuse. */
    {
        pid_t child = 0;
        char *const argv[] = {(char *)"aegir-conformance", nullptr};
        int const error = posix_spawn(&child, "/SCRATCH/POSIX.NOPE", nullptr, nullptr, argv, environ);
        if (error == 0) {
            (void)waitpid(child, nullptr, 0);
            deviation("posix_spawn", "a missing program is ENOENT", "posix_spawn.html names ENOENT");
        } else {
            conforms_or_deviation("posix_spawn", "a missing program is ENOENT", "posix_spawn.html",
                                  error == ENOENT, error, "ENOENT");
        }
    }

    /* system.html and popen.html run fork/exec and a pipe behind the call. */
    errno = 0;
    int const system_result = system("true");
    unavailable("system", "is not implemented", system_result == -1, errno);
    errno = 0;
    FILE *const pipe_stream = popen("true", "r");
    unavailable("popen", "is not implemented", pipe_stream == nullptr, errno);
    if (pipe_stream != nullptr) {
        (void)pclose(pipe_stream);
    }

    /* wait.html/waitid.html ERRORS: "[ECHILD] The calling process has no
     * existing unwaited-for child processes." */
    {
        int status = 0;
        errno = 0;
        int const waited = wait(&status);
        int const waited_errno = errno;
        conforms_or_deviation("wait", "no child is ECHILD", "wait.html",
                              waited == -1 && waited_errno == ECHILD, waited_errno, "ECHILD");
        siginfo_t info;
        errno = 0;
        int const waited_id = waitid(P_ALL, 0, &info, WEXITED);
        int const waited_id_errno = errno;
        conforms_or_deviation("waitid", "no child is ECHILD", "waitid.html",
                              waited_id == -1 && waited_id_errno == ECHILD, waited_id_errno,
                              "ECHILD");
    }

    /* The identifiers: pages with no error case, so a value or a hole. */
    errno = 0;
    answers_or_hole("getpid", "answers the process id", getpid() != (pid_t)-1, errno);
    errno = 0;
    answers_or_hole("getppid", "answers the parent process id", getppid() != (pid_t)-1, errno);
    errno = 0;
    answers_or_hole("getuid", "answers the real user id", getuid() != (uid_t)-1, errno);
    errno = 0;
    answers_or_hole("geteuid", "answers the effective user id", geteuid() != (uid_t)-1, errno);
    errno = 0;
    answers_or_hole("getgid", "answers the real group id", getgid() != (gid_t)-1, errno);
    errno = 0;
    answers_or_hole("getegid", "answers the effective group id", getegid() != (gid_t)-1, errno);
    errno = 0;
    answers_or_hole("getpgrp", "answers the process group", getpgrp() != (pid_t)-1, errno);
    errno = 0;
    answers_or_hole("getpgid", "answers a process group", getpgid(0) != (pid_t)-1, errno);
    errno = 0;
    answers_or_hole("getsid", "answers a session", getsid(0) != (pid_t)-1, errno);

    /* setuid.html ERRORS: EINVAL, EPERM. setgid.html likewise; setpgid.html and
     * setsid.html: EACCES, EINVAL, EPERM. Unimplemented, so holes. */
    errno = 0;
    unavailable("setuid", "is not implemented", setuid(0) == -1, errno);
    errno = 0;
    unavailable("setgid", "is not implemented", setgid(0) == -1, errno);
    errno = 0;
    unavailable("setpgid", "is not implemented", setpgid(0, 0) == -1, errno);
    errno = 0;
    unavailable("setsid", "is not implemented", setsid() == -1, errno);

    /* getrlimit.html ERRORS: EINVAL, EPERM; getrusage.html: EINVAL;
     * times.html: EOVERFLOW; nice.html: EPERM. */
    {
        struct rlimit limit;
        errno = 0;
        unavailable("getrlimit", "is not implemented", getrlimit(RLIMIT_NOFILE, &limit) == -1,
                    errno);
        errno = 0;
        unavailable("setrlimit", "is not implemented", setrlimit(RLIMIT_NOFILE, &limit) == -1,
                    errno);
    }
    {
        struct rusage usage;
        errno = 0;
        unavailable("getrusage", "is not implemented", getrusage(RUSAGE_SELF, &usage) == -1, errno);
    }
    {
        struct tms ticks;
        errno = 0;
        answers_or_hole("times", "answers the elapsed time", times(&ticks) != (clock_t)-1, errno);
    }
    errno = 0;
    unavailable("nice", "is not implemented", nice(0) == -1, errno);

    /* umask.html defines no error: it answers the previous mask, so the mask
     * must round-trip. */
    {
        mode_t const old = umask(022);
        mode_t const after = umask(0);
        if (old == (mode_t)-1) {
            errno = 0;
            unavailable("umask", "is not implemented", true, errno);
        } else {
            check("umask", "the previous mask round-trips", old == 022);
        }
        umask(after);
    }

    /* isatty.html/ttyname.html: the conformance service's descriptors are the
     * serial, not a terminal, so zero and a null pointer are the conforming
     * answers (a terminal would be 1 and its name). */
    check("isatty", "a non-terminal answers zero", isatty(0) == 0);
    check("ttyname", "a non-terminal answers null", ttyname(0) == nullptr);

    /* sysconf.html ERRORS: "[EINVAL] The value of the name argument is
     * invalid." uname.html defines no error. */
    check("sysconf", "a page size is answered", sysconf(_SC_PAGESIZE) > 0);
    errno = 0;
    check("sysconf", "an invalid name is EINVAL", sysconf(-1) == -1 && errno == EINVAL);
    {
        struct utsname name;
        check("uname", "the system is named", uname(&name) == 0 && name.sysname[0] != '\0');
    }
}

}  // namespace aegir_posix_conformance
