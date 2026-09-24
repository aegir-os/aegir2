/*
 * The terminal's spawn kit, adopted (specs/authority.md, specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "spawn_kit.h"

#include <aegir/bootstrap.h>
#include <aegir/trinket/application.h>

namespace aegir::terminal {

bool SpawnKit::adopt(aegir::trinket::Application& app)
{
    if (ready_) {
        return true;
    }

    /* The untyped auth carved for the session's commands. Its physical base is
     * not recorded: nothing tier 1 spawns is pointed at by a device, so the
     * allocator only needs a size to retype objects from. */
    uint64_t untyped_slot = 0;
    uint32_t untyped_bits = 0;
    if (!aegir::bootstrap::capability("spawn-untyped", 13, &untyped_slot) ||
        !aegir::bootstrap::capability_size_bits("spawn-untyped", 13, &untyped_bits) ||
        !app.allocator().adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                       0)) {
        return false;
    }

    uint64_t asid_pool = 0;
    if (!aegir::bootstrap::capability("asid-pool", 9, &asid_pool)) {
        return false;
    }
    asid_pool_ = static_cast<seL4_CPtr>(asid_pool);

    /* The terminal does not carry the whole initrd: it is 5.7 MiB, too much to
     * map into any child, and the spec's shape is narrower -- the shell reads
     * the one command's bytes from Initrd: through the namespace and hands the
     * spawner `Request.binary_image` (specs/shell.md). The spawner insists on
     * an Initrd reference it never reads when an image is given. */
    initrd_ = std::make_unique<aegir::spawn::Initrd>(nullptr, 0);

    /* The unbadged copies a command's own caps are minted from. A badged
     * endpoint cap cannot be minted again, so a spawner is handed the copy that
     * has never carried a badge (specs/authority.md). */
    uint64_t log_slot = 0;
    uint64_t nmspace_slot = 0;
    if (!aegir::bootstrap::capability("spawn:log.main", 14, &log_slot) ||
        !aegir::bootstrap::capability("spawn:vfs.namespace", 19, &nmspace_slot)) {
        return false;
    }
    log_port_ = static_cast<seL4_CPtr>(log_slot);
    nmspace_port_ = static_cast<seL4_CPtr>(nmspace_slot);

    /* The terminal's own con.stream endpoint and a fault endpoint for its
     * children, retyped from the untyped it just adopted. The terminal serves
     * the first and mints each command a caller copy of it. */
    seL4_Error error = seL4_NoError;
    stream_endpoint_ = app.allocator().alloc_object(seL4_EndpointObject,
                                                    seL4_EndpointBits, account_, &error);
    fault_endpoint_ = app.allocator().alloc_object(seL4_EndpointObject,
                                                   seL4_EndpointBits, account_, &error);
    if (stream_endpoint_ == 0 || fault_endpoint_ == 0) {
        return false;
    }

    arena_ = std::make_unique<aegir::mem::Arena>(app.allocator(), app.scratch(), account_);
    spawner_ = std::make_unique<aegir::spawn::Spawner>(
        app.allocator(), app.scratch(), *arena_, *initrd_, asid_pool_,
        static_cast<seL4_CPtr>(aegir::bootstrap::kSlotOwnCNode),
        aegir::bootstrap::kCNodeBits);
    ready_ = true;
    return true;
}

}  // namespace aegir::terminal
