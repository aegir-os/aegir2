/*
 * Turning the manifest into processes.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Director's part of specs/services.md: read what the manifest declares, check it
 * against the image, and create what it asks for (specs/director.md's step three).
 *
 * What is here now: the binary check -- an entry whose binary the image does not
 * contain stops the boot, because starting a system around a hole is worse than
 * not starting it -- and creation in declaration order.
 *
 * What is not here yet, and is deliberately not pretended: the *port* graph. The
 * manifest can declare `owns` and `needs`, and the spec makes those the creation
 * order and the failure graph; neither is enforced until there are ports to
 * resolve (the logger and Aegir's IPC are the next step, specs/services.md). With
 * one service and no ports the distinction does not yet bite, and a checker that
 * cannot be exercised is worse than a missing one.
 */

#ifndef AEGIR_DIRECTOR_SERVICES_H
#define AEGIR_DIRECTOR_SERVICES_H

#include <aegir/manifest.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/vspace.h>
#include <aegir/spawn/initrd.h>
#include <aegir/spawn/process.h>

#include "ports.h"
#include "supervisor.h"
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::director {

/** A service that is running, as the report and the supervisor need it. */
struct Started {
    char const *name;
    uint32_t name_length;
    /** The notification it will signal when it has finished starting. */
    seL4_CPtr supervision;
    /** Its own TCB, which is what a supervisor needs to stop it. */
    seL4_CPtr tcb;
    /** The badge every other service sees when this one calls. */
    uint64_t badge;
    uint64_t entry;
};

/** How the boot set went. */
struct Boot {
    unsigned declared;
    unsigned started;
    char const *problem; /* the empty string when every entry started */
};

class Services {
public:
    Services(mem::Allocator &allocator, mem::Scratch &scratch, mem::Arena &arena,
             spawn::Initrd const &initrd) noexcept;

    /** Check the manifest against the image, then create what it declares.
     *  `started` needs room for `manifest.size()` entries. */
    /** Create the shared fault endpoint: one for every service, badged per child
     *  (specs/director.md). Must happen before anything is started. */
    bool prepare(mem::Account &account) noexcept;

    /** `supervisor`, when given, is told about each service as it is created --
     *  before it can fault, rather than after. */
    /** `devices`/`devices_bytes`, when given, is a blob every service is handed
     *  through the bootstrap block: the machine's own description of itself, which
     *  is public information the device manager owns (specs/services.md). */
    void boot(manifest::Manifest const &manifest, mem::Account &account, Started *started,
              Boot &boot, Supervisor *supervisor, void const *devices,
              uint32_t devices_bytes, seL4_CPtr device_frame,
              uint32_t device_bytes, uint64_t device_physical,
              spawn::PortGrant const *extra, uint32_t extra_count) noexcept;

    seL4_CPtr fault_endpoint() const noexcept { return fault_endpoint_; }

    /** The ports the manifest declares, for whoever wants to report them. */
    PortGraph const &graph() const noexcept { return graph_; }

private:
    mem::Allocator &allocator_;
    /* Where the merged grant list for a service is built. A capability director
     * delegates is not a port, so it is not in the manifest's port graph, and the
     * list the spawner is given has to hold both (specs/authority.md). */
    mem::Arena &arena_;
    spawn::Initrd const &initrd_;
    seL4_CPtr fault_endpoint_;
    PortGraph graph_;
    spawn::Spawner spawner_;
};

}  // namespace aegir::director

#endif  // AEGIR_DIRECTOR_SERVICES_H
