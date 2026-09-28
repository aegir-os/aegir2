/*
 * The launch protocol: starting a program from a session (specs/launch.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One header, no code, the same shape as aegir/console_stream.h: a launcher
 * serves this, a program that launches includes it and
 * aegir/launch_client.h to call. A launch is the fork/exec analogue Aegir
 * lacks -- a program asks for another to be created and carries its own
 * context -- but the mechanism is a spawn out of authority the launcher was
 * delegated (specs/authority.md), so the request is a message, not a kernel
 * call.
 *
 * The wire vocabulary is here and deliberately depends on nothing: the
 * launcher's request handler is host-tested, so it must not pull a kernel
 * header. The client's call helpers, which do need the ipc envelope, are
 * their own header: aegir/launch_client.h. Strings travel in the namespace
 * protocol's shape (aegir/nmspace.h), as con.stream's do.
 */

#ifndef AEGIR_LAUNCH_H
#define AEGIR_LAUNCH_H

#include <stdint.h>

namespace aegir::launch {

/** The launcher port's name, in the class.instance shape every port is named
 *  in. One launcher per session serves it; the terminal serves it for the
 *  first cut (specs/launch.md). A program finds it through its bootstrap
 *  block, the way it finds con.stream. */
constexpr char const kPortName[] = "launch.session";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

/** What kind of program is being started -- what the launcher must hand it
 *  (specs/launch.md's kinds). A command shares the launcher's console stream
 *  and is handed its own console.gui, so it opens a window whenever it wants
 *  one; a launching program is given a spawn kit of its own and may launch in
 *  turn. The launcher does not classify a program as "windowed" before it
 *  runs: the program decides, by attaching. */
constexpr uint64_t kKindCommand = 1;   /* shares the launcher's console stream */
constexpr uint64_t kKindLaunching = 3; /* also launches programs */

/** Flags a spawn request carries. Bit 0 starts the command without waiting,
 *  the shell's `Run`: the caller draws its next prompt while it runs, and the
 *  command's exit is reaped without a `return code` (specs/shell.md). */
constexpr uint64_t kFlagBackground = 1;

/** Start one program. Fields after the method:
 *
 *    kind          one word (a kKind* value)
 *    flags         one word (kFlagBackground and friends)
 *    argv          a string: the command words NUL-separated, the program's
 *                  name first and the rest its arguments, already substituted
 *                  and quote-grouped by the caller (specs/shell.md)
 *    cwd           a string: the caller's current directory
 *    environment   a string: the caller's environment, NUL-separated
 *                  `NAME=VALUE` entries
 *    path          a string: the caller's search path, or empty
 *    std_in        a string: the redirected standard input, empty for the
 *                  launcher's console stream (specs/shell.md)
 *    std_out       a string: the redirected standard output, empty for the
 *                  console
 *    window        a string: an Amiga window specification
 *                  (`CON:x/y/w/h/title/...`), empty for none
 *    stack_pages   one word: the stack to ask for in pages, 0 for the
 *                  spawner's default
 *
 *  Answer: one word, 1 started and 0 refused -- a launcher that does not know
 *  the kind refuses rather than guessing -- plus, for a launcher that started
 *  commands, their badges after it, so the caller that relays the request
 *  knows what to reap (specs/memory.md Phase 5).
 *
 *  The launcher shares the terminal's endpoint in the first cut (there is one
 *  served port per process), so these method numbers start clear of
 *  con.stream's 1..11 rather than at 1. When the launcher moves to its own
 *  endpoint they are still valid; only the collision goes. */
constexpr uint32_t kMethodSpawn = 20;

/** Start a pipeline (specs/pipe.md): every stage at once, connected by pipes
 *  the launcher names. Fields after the method:
 *
 *    kind          one word (kKindCommand)
 *    flags         one word
 *    stage_count   one word
 *    per stage:    argv, std_in, std_out (strings, as in `spawn`)
 *    cwd, environment, path (strings, once for the whole pipeline)
 *    stack_pages   one word
 *
 *  Answer: one word, 1 started and 0 refused. The stages are kind-1 commands
 *  sharing the launcher's console stream at the ends; the launcher names the
 *  pipes between them, so a pipeline never depends on a name the caller
 *  chose. */
constexpr uint32_t kMethodPipeline = 21;

/** Reap one command (specs/launch.md): the stream that owns the command has
 *  seen it exit, so the launcher takes it back -- suspend it, release its
 *  memory, return its pool slots (specs/memory.md Phase 5). Fields after the
 *  method:
 *
 *    badge         one word: the command's badge, as its exit reported it
 *
 *  Answer: one word, 1 released and 0 for a badge no live command carries. */
constexpr uint32_t kMethodRelease = 22;

}  // namespace aegir::launch

#endif  // AEGIR_LAUNCH_H
