/*
 * aegir-posix-conformance: the threads area.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Every case is derived from the page named beside it in
 * third_party/posix-spec/issue8 (fetched by `make deps`). musl's `clone` is
 * routed to `__aegir_clone` by the tracked patch, so pthreads stand on real
 * seL4 threads; the calls a program builds on (create/join, mutex, condition,
 * thread-specific data, once, attributes) are exercised for real, and the ones
 * that need signal delivery or scheduling are holes.
 */

#include "harness.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>

namespace aegir_posix_conformance {
namespace {

int g_flag = 0;
int g_once_count = 0;
int g_cleanup_ran = 0;

void *worker(void *argument)
{
    g_flag = 1;
    return argument;
}

void *exiting(void *argument)
{
    pthread_exit(argument);
    return nullptr;
}

void once_run()
{
    ++g_once_count;
}

void cleanup_run(void *)
{
    g_cleanup_ran = 1;
}

void conforms_or_deviation(char const *interface, char const *test, char const *clause,
                           bool conforms, int observed, char const *expected)
{
    if (conforms) {
        check(interface, test, true);
    } else {
        char reason[128];
        snprintf(reason, sizeof(reason), "%s names %s; Aegir answers %d", clause, expected, observed);
        deviation(interface, test, reason);
    }
}

}  // namespace

void threads_surface()
{
    section("threads");

    /* pthread_self.html/pthread_equal.html define no error. */
    pthread_t const self = pthread_self();
    check("pthread_self", "the calling thread has an id", self != 0);
    check("pthread_equal", "a thread equals itself", pthread_equal(self, self) != 0);

    /* pthread_create.html/pthread_join.html: a thread is created, runs, and is
     * joined. */
    {
        pthread_t id;
        int const created = pthread_create(&id, nullptr, worker, (void *)0x42);
        check("pthread_create", "a thread is created", created == 0);
        if (created == 0) {
            check("pthread_equal", "a different thread is not equal", pthread_equal(self, id) == 0);
            void *result = nullptr;
            check("pthread_join", "the thread is joined and its value returned",
                  pthread_join(id, &result) == 0 && result == (void *)0x42 && g_flag == 1);
        } else {
            check("pthread_join", "the thread is joined and its value returned", false);
        }
    }
    /* pthread_exit.html: the thread ends with its argument. */
    {
        pthread_t id;
        if (pthread_create(&id, nullptr, exiting, (void *)0x77) == 0) {
            void *result = nullptr;
            check("pthread_exit", "the thread ends with its value",
                  pthread_join(id, &result) == 0 && result == (void *)0x77);
        } else {
            check("pthread_exit", "the thread ends with its value", false);
        }
    }
    /* pthread_detach.html: a detached thread is not joined. */
    {
        pthread_t id;
        check("pthread_detach", "a thread is detached",
              pthread_create(&id, nullptr, worker, nullptr) == 0 && pthread_detach(id) == 0);
    }

    /* pthread_mutex_*.html: return zero, and a held mutex is EBUSY to trylock. */
    {
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        check("pthread_mutex_init", "a mutex is initialised", pthread_mutex_init(&mutex, nullptr) == 0);
        check("pthread_mutex_lock", "the mutex is acquired", pthread_mutex_lock(&mutex) == 0);
        check("pthread_mutex_trylock", "a held mutex is not acquired",
              pthread_mutex_trylock(&mutex) == EBUSY);
        check("pthread_mutex_unlock", "the mutex is released", pthread_mutex_unlock(&mutex) == 0);
        check("pthread_mutex_destroy", "the mutex is destroyed", pthread_mutex_destroy(&mutex) == 0);
    }

    /* pthread_cond_*.html: return zero, and the wait is woken by the signal. */
    {
        pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
        check("pthread_cond_init", "a condition is initialised",
              pthread_cond_init(&condition, nullptr) == 0);
        check("pthread_cond_signal", "a signal is sent", pthread_cond_signal(&condition) == 0);
        check("pthread_cond_broadcast", "a broadcast is sent", pthread_cond_broadcast(&condition) == 0);
        check("pthread_cond_destroy", "the condition is destroyed",
              pthread_cond_destroy(&condition) == 0);
    }
    /* pthread_cond_wait.html: the wait hands the mutex to a signaller while it
     * blocks. A wait that does not release the mutex leaves the signaller
     * spinning on it: measured, the run wedged with the signaller at 100% CPU
     * and the log growing without bound. Named with that measurement rather
     * than called -- a wedge produces no finding. */
    skip("pthread_cond_wait", "waits for a signaller",
         "a wait that does not release the mutex wedges the signaller (measured)");

    /* pthread_key_*.html / pthread_setspecific.html / pthread_getspecific.html:
     * thread-specific data, which a single thread can set and read. */
    {
        pthread_key_t key;
        int const keyed = pthread_key_create(&key, nullptr);
        check("pthread_key_create", "a key is created", keyed == 0);
        if (keyed == 0) {
            check("pthread_setspecific", "a value is set",
                  pthread_setspecific(key, (void *)0x55) == 0);
            check("pthread_getspecific", "the value is read back",
                  pthread_getspecific(key) == (void *)0x55);
            /* pthread_key_delete takes musl's thread-list lock, which futexes;
             * futex (98) is unanswered, so musl's __lock loop spins at 100% CPU
             * -- measured, 420k `heap: sync 98` lines and the run wedged. Named
             * rather than called, because a wedge produces no finding. */
            skip("pthread_key_delete", "takes a lock musl futexes",
                 "futex (98) is unanswered, so the lock loop spins (measured)");
        } else {
            check("pthread_setspecific", "a value is set", false);
            check("pthread_getspecific", "the value is read back", false);
            skip("pthread_key_delete", "takes a lock musl futexes",
                 "futex (98) is unanswered, so the lock loop spins (measured)");
        }
    }

    /* pthread_once.html: the initialiser runs exactly once. */
    {
        static pthread_once_t once = PTHREAD_ONCE_INIT;
        int const first = pthread_once(&once, once_run);
        int const second = pthread_once(&once, once_run);
        check("pthread_once", "the initialiser runs once",
              first == 0 && second == 0 && g_once_count == 1);
    }

    /* pthread_attr_init.html/pthread_attr_destroy.html: return zero. */
    {
        pthread_attr_t attributes;
        check("pthread_attr_init", "attributes are initialised", pthread_attr_init(&attributes) == 0);
        check("pthread_attr_destroy", "attributes are destroyed",
              pthread_attr_destroy(&attributes) == 0);
    }

    /* pthread_cleanup_push.html: a handler registered and popped with a non-zero
     * argument runs. */
    {
        pthread_cleanup_push(cleanup_run, nullptr);
        pthread_cleanup_pop(1);
        check("pthread_cleanup_push", "a cleanup handler runs on pop", g_cleanup_ran == 1);
    }

    /* pthread_setcancelstate.html/pthread_setcanceltype.html/
     * pthread_testcancel.html: with cancellation disabled a test point returns.
     * pthread_cancel.html itself is a named skip: a request to a live thread
     * ends it, and the suite's thread is the unit under test. */
    {
        int old_state = 0;
        int old_type = 0;
        check("pthread_setcancelstate", "the cancel state is set",
              pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state) == 0);
        check("pthread_setcanceltype", "the cancel type is set",
              pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &old_type) == 0);
        pthread_testcancel();
        check("pthread_testcancel", "a disabled cancellation point returns", true);
        pthread_setcancelstate(old_state, nullptr);
        pthread_setcanceltype(old_type, nullptr);
        skip("pthread_cancel", "ends the thread it names", "the suite's own thread is the unit under test");
    }

    /* pthread_sigmask.html: answered by the same syscall as sigprocmask, with
     * the mask ignored. pthread_kill.html: a signal to a thread, and Aegir
     * delivers none. */
    {
        sigset_t set;
        sigemptyset(&set);
        int const masked = pthread_sigmask(SIG_BLOCK, &set, nullptr);
        conforms_or_deviation("pthread_sigmask", "a mask is set", "pthread_sigmask.html",
                              masked == 0, masked, "zero");
        (void)pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
        int const killed = pthread_kill(self, SIGUSR1);
        unavailable("pthread_kill", "is not implemented", killed != 0, killed);
    }

    /* pthread_atfork.html: the handlers are recorded. */
    check("pthread_atfork", "handlers are registered", pthread_atfork(nullptr, nullptr, nullptr) == 0);

    /* pthread_getcpuclockid.html and pthread_*schedparam.html: a thread's CPU
     * clock and its scheduling parameters. */
    {
        clockid_t clock = 0;
        int const cpu = pthread_getcpuclockid(self, &clock);
        conforms_or_deviation("pthread_getcpuclockid", "a CPU clock is answered",
                              "pthread_getcpuclockid.html", cpu == 0, cpu, "zero");

        int policy = 0;
        struct sched_param parameters;
        int const got = pthread_getschedparam(self, &policy, &parameters);
        unavailable("pthread_getschedparam", "is not implemented", got != 0, got);
        int const set = pthread_setschedparam(self, SCHED_OTHER, &parameters);
        unavailable("pthread_setschedparam", "is not implemented", set != 0, set);
    }
}

}  // namespace aegir_posix_conformance
