/*
 * The launch API: starting a program from a program (specs/launch.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The C++ face is `aegir::launch`, the fork/exec analogue Aegir lacks; the C
 * runtime primitive underneath it is `aegir_spawn` (and the raw
 * `aegir_launch_request` it builds on), so a developer chooses which to call.
 * Neither serializes the caller's context by hand: the runtime fills the
 * request from the process's own state -- its environment, its current
 * directory, its path, its stack ask -- exactly as `fork` hands a child the
 * parent's (specs/launch.md).
 *
 * A launch goes to the session's launcher, found by name in the bootstrap
 * block, so the call site does not name a port and does not change when the
 * launcher moves out (specs/launch.md).
 */

#ifndef AEGIR_LAUNCH_CLIENT_H
#define AEGIR_LAUNCH_CLIENT_H

#include <aegir/launch.h>
#include <aegir/ipc/port.h>

#include <string>

namespace aegir::launch {

/** The session's launcher port, found under `launch.session` through the
 *  bootstrap block. Invalid when the process was given none; a launch then
 *  fails rather than guessing. */
aegir::ipc::Consumer launcher() noexcept;

/** The caller's own context, read from the runtime: the current directory it
 *  is tracking, its environment as NUL-separated `NAME=VALUE` entries, its
 *  path, and the stack it asks a program run for. What inheritance is made
 *  of (specs/launch.md); a caller never assembles it. */
struct Context {
    std::string cwd;
    std::string environment;
    std::string path;
    uint32_t stack_pages = 0;
};

Context caller_context();

/** One pipeline stage (specs/pipe.md): the command words NUL-separated,
 *  program first, and the stage's own redirections, empty for the console or
 *  the connecting pipe. */
struct Stage {
    char const *argv;
    uint32_t argv_length;
    char const *std_in;
    uint32_t std_in_length;
    char const *std_out;
    uint32_t std_out_length;
};

/** Start a command (kind 1) that shares the launcher's console stream.
 *  `argv` is the command words NUL-separated, program first; `std_in` and
 *  `std_out` are VFS paths, empty for the console. With `background` the
 *  caller does not wait (the shell's `Run`). True when it started. */
bool command(char const *argv, uint32_t argv_length, char const *std_in,
             uint32_t std_in_length, char const *std_out, uint32_t std_out_length,
             bool background);

/** Start a pipeline (specs/pipe.md): every stage at once, connected by pipes
 *  the launcher names. True when it started. */
bool pipeline(Stage const *stages, uint32_t count);

/** Start one program of any kind (specs/launch.md's kinds). `window` is an
 *  Amiga window specification, empty for none. Kind 1 is `command`; kinds 2
 *  and 3 are the launcher's to fulfill. True when it started. */
bool spawn(char const *argv, uint32_t argv_length, uint64_t kind, char const *window,
           uint32_t window_length);

extern "C" {

/** The C runtime's primitive: send a launch request -- `method` and the packed
 *  words of aegir/launch.h -- on the session's launcher port. Answers 1
 *  started, 0 refused, and -1 when the process has no launcher or the call
 *  itself was refused. `aegir::launch` builds its words and calls this. */
int aegir_launch_request(uint32_t method, uint64_t const *words, uint32_t count) noexcept;

/** The ergonomic C face of a launch: `argv` is a null-terminated argument
 *  vector, argv[0] the program's name, and the caller's context is inherited.
 *  `window` is an Amiga specification or null, and `kind` is a kKind* value.
 *  Answers as `aegir_launch_request`. */
int aegir_spawn(char *const argv[], char const *std_in, char const *std_out,
                int background, char const *window, int kind) noexcept;

}  // extern "C"

}  // namespace aegir::launch

#endif  // AEGIR_LAUNCH_CLIENT_H
