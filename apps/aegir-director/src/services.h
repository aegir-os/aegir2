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
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::director {

/** A service that is running, as the report and the supervisor need it. */
struct Started {
    char const *name;
    uint32_t name_length;
    /** The notification it will signal when it has finished starting. */
    seL4_CPtr supervision;
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
    void boot(manifest::Manifest const &manifest, mem::Account &account, Started *started,
              Boot &boot) noexcept;

private:
    spawn::Initrd const &initrd_;
    spawn::Spawner spawner_;
};

}  // namespace aegir::director

#endif  // AEGIR_DIRECTOR_SERVICES_H
