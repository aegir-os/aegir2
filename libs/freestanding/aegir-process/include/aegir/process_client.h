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

/** `text` (a `length`-byte view, or null) into `out`, bounded by `capacity` and
 *  NUL-terminated. */
inline void set_field(char *out, uint32_t capacity, char const *text, uint32_t length) noexcept
{
    uint32_t at = 0;
    if (text != nullptr) {
        for (; at < length && at + 1 < capacity; ++at) {
            out[at] = text[at];
        }
    }
    out[at] = '\0';
}

}  // namespace detail

/** Register one process. `pid` is the child's badge, `parent` the spawner's
 *  (the parent pid), and `name`/`path` the program's name and full path as
 *  views (either may be null or empty). False when there is no registry to call
 *  or it refused. */
inline bool register_process(aegir::ipc::Consumer const &registry, uint64_t pid,
                             uint64_t parent, char const *name, uint32_t name_length,
                             char const *path, uint32_t path_length) noexcept
{
    if (!registry.valid()) {
        return false;
    }
    Row row{};
    row.pid = pid;
    row.parent = parent;
    detail::set_field(row.name, sizeof(row.name), name, name_length);
    detail::set_field(row.path, sizeof(row.path), path, path_length);
    uint64_t reply[1] = {0};
    aegir::ipc::WordsReply const answer =
        registry.call_words(kMethodRegister, reinterpret_cast<uint64_t const *>(&row),
                            kRowWords, reply, 1);
    return answer.error == 0 && answer.count >= 1 && reply[0] == kProcessAdded;
}

/** Register one process and hand the registry its **break source** as the
 *  call's one capability (specs/process.md): the spawner minted it from the
 *  child's notification, so `break` can wake the child. `source` of zero
 *  registers metadata alone -- a process the caller gives no way to wake, which
 *  the enforced halt (Phase 3) still reaches. */
inline bool register_process(aegir::ipc::Consumer const &registry, uint64_t pid,
                             uint64_t parent, char const *name, uint32_t name_length,
                             char const *path, uint32_t path_length,
                             seL4_CPtr source) noexcept
{
    if (!registry.valid()) {
        return false;
    }
    Row row{};
    row.pid = pid;
    row.parent = parent;
    detail::set_field(row.name, sizeof(row.name), name, name_length);
    detail::set_field(row.path, sizeof(row.path), path, path_length);
    uint64_t reply[1] = {0};
    bool cap_received = false;
    aegir::ipc::WordsReply const answer = registry.call_transfer(
        kMethodRegister, reinterpret_cast<uint64_t const *>(&row), kRowWords, source,
        reply, 1, &cap_received);
    if (cap_received) {
        /* The registry does not answer with a capability; clear anything that
         * arrived so the next transfer is not refused an occupied slot. */
        aegir::ipc::drop_received_cap();
    }
    return answer.error == 0 && answer.count >= 1 && reply[0] == kProcessAdded;
}

}  // namespace aegir::process

#endif  // AEGIR_PROCESS_CLIENT_H
