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
#include <aegir/spawn/pager.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::director {

/** The pager's per-service record (specs/pager.md): the child's VSpace root and
 *  its reserved stack range, filled by the boot thread before the child runs.
 *  `watched` is false for the services spawned before `mem.main` is up (the
 *  memory service itself, and its predecessors), which are not paged. */
struct PagerRecord {
    seL4_CPtr root = 0;
    uintptr_t base = 0;
    uintptr_t lo = 0;
    uintptr_t top = 0;
    uint64_t max = 0;
    bool watched = false;
};

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
    PagerRecord pager;
    /* The pager's live state, the supervisor's alone once the record is
     * published: the service's child-badged `mem.main` chunk in use, and what
     *  is left of it (bytes). */
    seL4_CPtr chunk = 0;
    uint64_t chunk_left = 0;
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
                char const *name, uint32_t name_length, PagerRecord const &pager) noexcept;

    /** The unbadged `mem.main` endpoint director holds for the port its graph
     *  built, so the pager can mint a copy badged with a child's id
     *  (specs/pager.md). Zero leaves every service unwatched. */
    void set_mem_main(seL4_CPtr endpoint) noexcept { mem_main_ = endpoint; }

    /** The pager (specs/pager.md): classify a fault against the service's stack
     *  and grow it from a child-badged mem.main chunk. The supervisor thread
     *  calls this; the boot thread never does. */
    using StackFault = spawn::StackFault;
    bool grow(uint64_t badge, uintptr_t address, StackFault *fault,
              char const **why) noexcept;

    unsigned faults() const noexcept;

    char const *problem() const noexcept { return problem_; }

private:
    /* The pager's frame source: ensure the service's chunk, retype one object
     * from it into a slot of the pager pool, and record it; `drop` gives a
     * redundant table's slot back (specs/pager.md). */
    seL4_CPtr pager_carve(uint64_t badge, seL4_Word type, seL4_Word size_bits,
                          seL4_Error *error) noexcept;
    void pager_drop(uint64_t badge, seL4_CPtr slot) noexcept;
    static seL4_CPtr pager_carve_cb(void *context, seL4_Word type, seL4_Word size_bits,
                                    seL4_Error *error) noexcept;
    static void pager_drop_cb(void *context, seL4_CPtr slot) noexcept;
    Supervised *find(uint64_t badge) noexcept;
    mem::Allocator &allocator_;
    mem::Scratch &scratch_;
    mem::Arena &arena_;
    Supervised *table_;
    uint32_t capacity_;
    uint32_t *count_;
    unsigned *faults_;
    seL4_CPtr mem_main_ = 0;
    char const *problem_;
};

}  // namespace aegir::director

#endif  // AEGIR_DIRECTOR_SUPERVISOR_H
