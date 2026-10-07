/*
 * aegir-posix-child: a child for the POSIX process acceptance (specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The plainest hosted program there is: it stands the runtime up and returns
 * from `main`, so its runtime reports the exit its parent's `wait4` completes
 * on. It exists as its own program, with its own name, because the launcher
 * announces every started command as `command started <argv[0]>` and the
 * acceptance fires a step on that cue: a second command sharing a cue would
 * fire the step on the first of them (scripts/run_target.py, AGENTS.md). A
 * child named `date` did exactly that to the acceptance's own `date` step.
 */

#include <aegir/command.h>

#include <cstdlib>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    /* The runtime's stand-up -- the crt0 role, which belongs in the runtime and
     * is the next step; here it is the one line this child carries. */
    if (!aegir::command::start("posix-child")) {
        std::_Exit(127);
    }
    return 0;
}
