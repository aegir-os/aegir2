/*
 * aegir-posix-conformance: the signals area.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Every case is derived from the page named beside it in
 * third_party/posix-spec/issue8 (fetched by `make deps`). Aegir delivers no
 * signals (specs/posix.md): the dispatcher answers rt_sigprocmask and ignores
 * the mask, and answers nothing else in the family. The set-manipulation calls
 * and sigsetjmp/siglongjmp are libc and conform; the calls that would suspend
 * until a signal arrives are named skips, because calling them would wedge the
 * run.
 */

#include "harness.h"

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

namespace aegir_posix_conformance {
namespace {

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

void signals_surface()
{
    section("signals");

    /* sigemptyset.html/sigfillset.html/sigaddset.html/sigdelset.html/
     * sigismember.html: the set itself, which is libc and conforms. */
    sigset_t set;
    errno = 0;
    check("sigemptyset", "the set is emptied",
          sigemptyset(&set) == 0 && sigismember(&set, SIGUSR1) == 0);
    errno = 0;
    check("sigaddset", "a signal is added",
          sigaddset(&set, SIGUSR1) == 0 && sigismember(&set, SIGUSR1) == 1);
    check("sigismember", "membership is answered", sigismember(&set, SIGUSR1) == 1);
    errno = 0;
    check("sigdelset", "a signal is removed",
          sigdelset(&set, SIGUSR1) == 0 && sigismember(&set, SIGUSR1) == 0);
    errno = 0;
    check("sigfillset", "the set is filled",
          sigfillset(&set) == 0 && sigismember(&set, SIGUSR1) == 1);

    /* sigprocmask.html RETURN VALUE (zero) and ERRORS ("[EINVAL] ... the value
     * of the how argument is not equal to one of the defined values"). */
    sigemptyset(&set);
    errno = 0;
    check("sigprocmask", "a mask is set", sigprocmask(SIG_BLOCK, &set, nullptr) == 0);
    errno = 0;
    int const bad_how = sigprocmask(-1, &set, nullptr);
    int const bad_how_errno = errno;
    conforms_or_deviation("sigprocmask", "an invalid how is EINVAL", "sigprocmask.html",
                          bad_how == -1 && bad_how_errno == EINVAL, bad_how_errno, "EINVAL");
    (void)sigprocmask(SIG_UNBLOCK, &set, nullptr);

    /* sigsetjmp.html/siglongjmp.html: a direct invocation is zero, and the jump
     * returns its value (libc's setjmp, no signal involved). */
    {
        sigjmp_buf environment;
        int const jumped = sigsetjmp(environment, 1);
        if (jumped == 0) {
            check("sigsetjmp", "a direct return is zero", true);
            siglongjmp(environment, 1);
        }
        check("siglongjmp", "the jump returns its value", jumped == 1);
    }

    /* sigaction.html ERRORS: "[EINVAL] The sig argument is not a valid signal
     * number or an attempt is made to catch a signal that cannot be caught". */
    {
        struct sigaction action;
        action.sa_handler = SIG_IGN;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        errno = 0;
        int const installed = sigaction(SIGUSR1, &action, nullptr);
        unavailable("sigaction", "is not implemented", installed == -1, errno);
    }
    /* sigpending.html defines no error. */
    {
        sigset_t pending;
        errno = 0;
        int const pend = sigpending(&pending);
        unavailable("sigpending", "is not implemented", pend == -1, errno);
    }
    /* sigtimedwait.html/sigwaitinfo.html: a zero timeout does not wait. */
    {
        struct timespec none = {0, 0};
        sigset_t watching;
        sigemptyset(&watching);
        errno = 0;
        int const timed = sigtimedwait(&watching, nullptr, &none);
        unavailable("sigtimedwait", "is not implemented", timed == -1, errno);
        errno = 0;
        int const informed = sigwaitinfo(&watching, nullptr);
        unavailable("sigwaitinfo", "is not implemented", informed == -1, errno);
    }
    /* sigqueue.html ERRORS: EAGAIN, EINVAL, EPERM, ESRCH. */
    {
        union sigval value;
        value.sival_int = 0;
        errno = 0;
        int const queued = sigqueue(getpid(), SIGUSR1, value);
        unavailable("sigqueue", "is not implemented", queued == -1, errno);
    }
    /* kill.html ERRORS: EINVAL, EPERM, ESRCH; killpg.html the same. */
    errno = 0;
    int const killed = kill(getpid(), SIGUSR1);
    unavailable("kill", "is not implemented", killed == -1, errno);
    errno = 0;
    int const killed_group = killpg(getpgrp(), SIGUSR1);
    unavailable("killpg", "is not implemented", killed_group == -1, errno);
    /* raise.html ERRORS: EINVAL. */
    errno = 0;
    int const raised = raise(SIGUSR1);
    unavailable("raise", "is not implemented", raised != 0, errno);
    /* alarm.html: "always successful", and answers the seconds a previous alarm
     * still had. */
    {
        unsigned const none = alarm(1);
        unsigned const pending = alarm(0);
        conforms_or_deviation("alarm", "a scheduled alarm is answered", "alarm.html",
                              none == 0 && pending != 0, 0, "the remaining seconds");
    }
    /* sigaltstack.html ERRORS: EINVAL, ENOMEM, EPERM. */
    {
        stack_t alternate;
        alternate.ss_sp = nullptr;
        alternate.ss_size = 0;
        alternate.ss_flags = 0;
        errno = 0;
        int const stacked = sigaltstack(&alternate, nullptr);
        unavailable("sigaltstack", "is not implemented", stacked == -1, errno);
    }
    /* signal.html ERRORS: EINVAL. */
    {
        errno = 0;
        void (*const previous)(int) = signal(SIGUSR1, SIG_IGN);
        unavailable("signal", "is not implemented", previous == SIG_ERR, errno);
    }

    /* The calls that suspend until a signal arrives. Aegir delivers none, so
     * calling them would wedge the run: named, with the reason. */
    skip("pause", "suspends until a signal", "Aegir delivers none, so the call would wedge");
    skip("sigsuspend", "suspends until a signal", "Aegir delivers none, so the call would wedge");
    skip("sigwait", "waits for a signal", "Aegir delivers none, so the call would wedge");
}

}  // namespace aegir_posix_conformance
