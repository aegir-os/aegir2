/*
 * The process registry's client half (specs/process.md): the spawner's one
 * call as it starts a child.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A process does not register itself. Whoever starts it registers it -- the
 * boot spawner for the services, the session's spawner for its services, a
 * launcher for its commands -- because the spawner is the one that knows the
 * child's pid, its parent, its name and its path, and the row is a fact about a
 * start (specs/process.md). This is that call, in one place so no spawner
 * rebuilds the row.
 *
 * It needs a caller half of process.registry; a spawner handed none registers
 * nothing, and a Break cannot name its children.
 */

#ifndef AEGIR_PROCESS_CLIENT_H
#define AEGIR_PROCESS_CLIENT_H

#include <aegir/ipc/port.h>
#include <aegir/process.h>

#include <stdint.h>

namespace aegir::process {

namespace detail {

/** `text` into `out`, bounded by `capacity` and NUL-terminated; empty for a
 *  null text. */
inline void copy_field(char *out, uint32_t capacity, char const *text) noexcept
{
    uint32_t at = 0;
    if (text != nullptr) {
        for (; text[at] != '\0' && at + 1 < capacity; ++at) {
            out[at] = text[at];
        }
    }
    out[at] = '\0';
}

}  // namespace detail

/** Register one process. `pid` is the child's badge, `parent` the spawner's
 *  (the parent pid), `name` the program name and `path` its full path (either
 *  may be null). False when there is no registry to call or it refused. */
inline bool register_process(aegir::ipc::Consumer const &registry, uint64_t pid,
                             uint64_t parent, char const *name, char const *path) noexcept
{
    if (!registry.valid()) {
        return false;
    }
    Row row{};
    row.pid = pid;
    row.parent = parent;
    detail::copy_field(row.name, sizeof(row.name), name);
    detail::copy_field(row.path, sizeof(row.path), path);
    uint64_t reply[1] = {0};
    aegir::ipc::WordsReply const answer =
        registry.call_words(kMethodRegister, reinterpret_cast<uint64_t const *>(&row),
                            kRowWords, reply, 1);
    return answer.error == 0 && answer.count >= 1 && reply[0] == kProcessAdded;
}

}  // namespace aegir::process

#endif  // AEGIR_PROCESS_CLIENT_H
