/*
 * aegir-posix-test: the POSIX process surface's acceptance client
 * (specs/posix.md's process sub-arc).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A plain C program: it starts a child with `posix_spawn` and waits for it with
 * `wait4`, carrying no Aegir-specific call. The runtime the program links
 * provides both -- `posix_spawn` over the launcher's spawn, and `wait4` over the
 * launcher's wait -- so the only evidence of Aegir here is that it works.
 */

#include <aegir/command.h>
#include <aegir/debug.h>

#include <cstdlib>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    /* The runtime's stand-up -- the crt0 role, which every hosted Aegir program
     * does in its main today and which belongs in the runtime, so that a plain
     * program needs no Aegir call at all. That move is the next step; here it is
     * the one line the acceptance carries. */
    if (!aegir::command::start("posix-test")) {
        std::_Exit(127);
    }
    aegir::debug_write("AEGIR_POSIX_START\n");

    char *child_argv[] = {const_cast<char *>("date"), nullptr};
    pid_t pid = 0;
    if (posix_spawn(&pid, "date", nullptr, nullptr, child_argv, environ) != 0) {
        aegir::debug_write("AEGIR_POSIX_SPAWN_FAIL\n");
        std::_Exit(1);
    }
    aegir::debug_write("AEGIR_POSIX_SPAWNED\n");

    int status = 0;
    if (wait4(pid, &status, 0, nullptr) != pid) {
        aegir::debug_write("AEGIR_POSIX_WAIT_FAIL\n");
        std::_Exit(1);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        aegir::debug_write("AEGIR_POSIX_WAIT_OK\n");
        std::_Exit(0);
    }
    aegir::debug_write("AEGIR_POSIX_STATUS_FAIL\n");
    std::_Exit(1);
}
