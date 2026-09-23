/*
 * The thread that watches the services.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * specs/director.md: faults arrive on one shared endpoint, badged with the
 * offender, so a single thread can watch every service and still know which one
 * it is looking at. It is a second thread because seL4 cannot wait on two
 * capabilities at once -- the boot thread has to be free to wait for services to
 * report ready while somebody else is listening for them to die.
 *
 * The thread shares director's CSpace and VSpace, so it needs its own IPC buffer
 * and stack and nothing else. Both come from the window the spawner fills frames
 * through and are simply never unmapped: a thread's IPC buffer has to stay where
 * it was configured, and the window is ours to leave things in.
 *
 * A badge the supervisor does not know is an unknown service rather than a guess:
 * a full table would be one that had to be sized before the manifest was read.
 */

#ifndef AEGIR_DIRECTOR_SUPERVISOR_H
#define AEGIR_DIRECTOR_SUPERVISOR_H

#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/vspace.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::director {

/** One supervised service, as the supervisor thread sees it. */
struct Supervised {
    uint64_t badge;
    seL4_CPtr tcb;
    /** The notification its supervisor waits on. Signalling it is how a death
     *  reaches the thread that is waiting to hear the service is ready: the child
     *  signals with its own badge, the supervisor with director's, so the two
     *  cannot be confused. */
    seL4_CPtr supervision;
    char const *name;
    uint32_t name_length;
};

class Supervisor {
public:
    Supervisor(mem::Allocator &allocator, mem::Scratch &scratch, mem::Arena &arena) noexcept;

    /** Create the thread and start it. `table` and `capacity` are the boot
     *  thread's, and the thread only ever reads them. */
    bool start(seL4_CPtr fault_endpoint, Supervised *table, uint32_t capacity,
               mem::Account &account) noexcept;

    /** The boot thread says who a badge belongs to. The count is published last,
     *  so a lookup never sees half a record. */
    void record(uint32_t index, uint64_t badge, seL4_CPtr tcb, seL4_CPtr supervision,
                char const *name, uint32_t name_length) noexcept;

    unsigned faults() const noexcept;

    char const *problem() const noexcept { return problem_; }

private:
    mem::Allocator &allocator_;
    mem::Scratch &scratch_;
    mem::Arena &arena_;
    Supervised *table_;
    uint32_t capacity_;
    uint32_t *count_;
    unsigned *faults_;
    char const *problem_;
};

}  // namespace aegir::director

#endif  // AEGIR_DIRECTOR_SUPERVISOR_H
