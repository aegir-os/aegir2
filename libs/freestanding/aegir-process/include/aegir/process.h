/*
 * The process registry's protocol (specs/process.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One header, no code, the shape of aegir/registry.h: the registry (a service,
 * aegir-process-registry) owns the port and serves it; a client -- the `Break`
 * command, or the spawn kit registering a process -- includes this to call. The
 * wide vocabulary is `aegir::ipc`'s multi-word envelope; the port's name is the
 * class.instance shape every port is named in.
 *
 * A process is named by its **pid**, which is the per-process badge the kernel
 * reports (specs/memory.md); a Row is what the registry knows about one. The
 * attention flags are the Amiga's Break (specs/process.md): C aborts, D halts a
 * script, E and F are reserved, and ALL is C..F. `break` sets flags on a pid;
 * setting C is what the shell later renders `***BREAK`.
 */

#ifndef AEGIR_PROCESS_H
#define AEGIR_PROCESS_H

#include <stdint.h>

namespace aegir::process {

/** The port's name, in the class.instance shape every port is named in. */
constexpr char kPortName[] = "process.registry";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

/** How many processes are live. Answer: one word. */
constexpr uint32_t kMethodCount = 1;

/** One process, by index into the live set. In: an index; answer: a Row's
 *  words, or the empty reply when the index is past the count. Walking `count`
 *  and `describe` is how a caller finds a pid by name (specs/process.md's
 *  `Break NAME`). */
constexpr uint32_t kMethodDescribe = 2;

/** Set attention flags on a pid. In: the pid, then the flags; answer: one word,
 *  1 set and 0 refused -- a pid the registry does not hold, or one the caller
 *  may not break (specs/process.md's authority). */
constexpr uint32_t kMethodBreak = 3;

/** Add a process to the live set. In: a Row's words, of which `pid`, `owner`,
 *  `name` and `path` are read and `flags`/`state` are the registry's own;
 *  answer: one word, 1 registered and 0 refused (a duplicate pid, or a full
 *  table). The spawn kit registers the metadata (specs/process.md). */
constexpr uint32_t kMethodRegister = 4;

/** Remove a process. In: the pid; answer: one word, 1 removed and 0 refused.
 *  A process leaves when it is gone, so its exit is what sends this. */
constexpr uint32_t kMethodUnregister = 5;

/** The attention flags (specs/process.md). C is the default: the flag Ctrl-C
 *  sets, and the one that aborts. D halts a running script file. E and F are
 *  reserved. */
constexpr uint64_t kAttnC = 1ULL << 0;
constexpr uint64_t kAttnD = 1ULL << 1;
constexpr uint64_t kAttnE = 1ULL << 2;
constexpr uint64_t kAttnF = 1ULL << 3;
constexpr uint64_t kAttnAll = kAttnC | kAttnD | kAttnE | kAttnF;

/** What a row's `state` says. `Break pending` is a flag set and not yet acted
 *  on; the delivery that clears it is specs/process.md's, later. */
constexpr uint64_t kStateRunning = 0;
constexpr uint64_t kStateBreakPending = 1;

/** What `break` answers. */
constexpr uint64_t kBreakRefused = 0;
constexpr uint64_t kBreakSet = 1;

/** What `register` and `unregister` answer. */
constexpr uint64_t kProcessRefused = 0;
constexpr uint64_t kProcessAdded = 1;

/** How much of a name and a path a Row holds, NUL-terminated within its field.
 *  The envelope's room, not a name bound: a longer one is truncated for the
 *  row, and the pattern match is over what fits. */
constexpr uint32_t kNameMax = 24;
constexpr uint32_t kPathMax = 72;

/** One process, as the registry knows it and as `describe` answers it. */
struct Row {
    uint64_t pid;   /* the process's badge, as the kernel reports it */
    uint64_t owner; /* the badge of whoever started it (its class, for the
                       authority check) */
    uint64_t flags; /* the attention flags currently set */
    uint64_t state; /* running, or a break pending */
    char name[kNameMax]; /* the program name, excluding its path */
    char path[kPathMax]; /* the program's full path, when the launcher knew it */
};

/** The Row as the message carries it. */
constexpr uint32_t kRowWords = (sizeof(Row) + 7) / 8;

}  // namespace aegir::process

#endif  // AEGIR_PROCESS_H
