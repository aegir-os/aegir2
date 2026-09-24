/*
 * The terminal's spawn kit, adopted (specs/authority.md, specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "spawn_kit.h"

#include <aegir/bootstrap.h>
#include <aegir/mem/vspace.h>
#include <aegir/trinket/application.h>

namespace aegir::terminal {

namespace {

/* The allocator over the command pool: static, like auth's session allocator,
 * because its node table and untyped table are far larger than a stack. It is
 * reset and re-adopted for each command (begin). */
aegir::mem::Allocator g_command_mem(nullptr);

/* The command allocator's node pool: the toolkit's window wires its own
 * allocator's pool, so a second allocator brings its own region. A command's
 * spawn splits a few hundred pieces; 64 KiB is beyond that and lives in BSS. */
alignas(64) unsigned char g_command_nodes[64 * 1024];

}  // namespace

bool SpawnKit::adopt(aegir::trinket::Application& app)
{
    if (ready_) {
        return true;
    }
    app_ = &app;

    /* The command pool auth carved for the session's commands. */
    uint64_t pool_slot = 0;
    uint32_t pool_bits = 0;
    if (!aegir::bootstrap::capability("command-pool", 12, &pool_slot) ||
        !aegir::bootstrap::capability_size_bits("command-pool", 12, &pool_bits)) {
        return false;
    }
    command_pool_ = static_cast<seL4_CPtr>(pool_slot);
    command_pool_bits_ = pool_bits;

    uint64_t asid_pool = 0;
    if (!aegir::bootstrap::capability("asid-pool", 9, &asid_pool)) {
        return false;
    }
    asid_pool_ = static_cast<seL4_CPtr>(asid_pool);

    /* The unbadged copies a command's own caps are minted from. A badged
     * endpoint cap cannot be minted again (specs/authority.md). */
    uint64_t log_slot = 0;
    uint64_t nmspace_slot = 0;
    if (!aegir::bootstrap::capability("spawn:log.main", 14, &log_slot) ||
        !aegir::bootstrap::capability("spawn:vfs.namespace", 19, &nmspace_slot)) {
        return false;
    }
    log_port_ = static_cast<seL4_CPtr>(log_slot);
    nmspace_port_ = static_cast<seL4_CPtr>(nmspace_slot);

    /* The terminal's own con.stream endpoint and a fault endpoint for its
     * children, retyped from the toolkit's memory (they live as long as the
     * terminal, not as long as a command). */
    seL4_Error error = seL4_NoError;
    stream_endpoint_ = app.allocator().alloc_object(seL4_EndpointObject,
                                                    seL4_EndpointBits, account_, &error);
    fault_endpoint_ = app.allocator().alloc_object(seL4_EndpointObject,
                                                   seL4_EndpointBits, account_, &error);
    if (stream_endpoint_ == 0 || fault_endpoint_ == 0) {
        return false;
    }

    g_command_mem.adopt_nodes(g_command_nodes, sizeof(g_command_nodes));
    /* The spawner insists on an Initrd it never reads when an image is given:
     * the shell hands the one command's bytes as `binary_image`
     * (specs/shell.md). */
    initrd_ = std::make_unique<aegir::spawn::Initrd>(nullptr, 0);
    ready_ = true;
    return true;
}

aegir::mem::Allocator& SpawnKit::memory()
{
    return g_command_mem;
}

bool SpawnKit::begin()
{
    if (!ready_) {
        return false;
    }
    g_command_mem.reset();
    if (!g_command_mem.adopt_untyped(command_pool_, command_pool_bits_, 0)) {
        return false;
    }
    g_command_mem.adopt_slots(app_->spawn_slot_base(), app_->spawn_slot_count(), 0);
    /* The staging mark is taken here, after every permanent map the toolkit
     * made; the reclaim rewinds to it (specs/auth.md's reclaim shape). */
    scratch_mark_ = app_->scratch().next();
    arena_ = std::make_unique<aegir::mem::Arena>(g_command_mem, app_->scratch(), account_);
    spawner_ = std::make_unique<aegir::spawn::Spawner>(
        g_command_mem, app_->scratch(), *arena_, *initrd_, asid_pool_,
        static_cast<seL4_CPtr>(aegir::bootstrap::kSlotOwnCNode),
        aegir::bootstrap::kCNodeBits);
    return true;
}

void SpawnKit::reclaim()
{
    spawner_.reset();
    arena_.reset();
    /* The revoke is the memory's way back: every object the command was was
     * retyped from the pool, and with them go the caps it held. The reserved
     * slots are empty afterwards, so the cursor returns to the base. */
    seL4_CNode_Revoke(aegir::bootstrap::kSlotOwnCNode, command_pool_,
                      aegir::bootstrap::kCNodeBits);
    g_command_mem.slot_release(app_->spawn_slot_base());
    app_->scratch().rewind(scratch_mark_);
}

void SpawnKit::finish(seL4_CPtr tcb)
{
    /* Stop the command before its capabilities go: a running thread whose TCB
     * is revoked is undefined. The command has already halted on its own; the
     * suspend is what makes that certain. */
    if (tcb != 0) {
        seL4_TCB_Suspend(tcb);
    }
    reclaim();
}

void SpawnKit::abort()
{
    reclaim();
}

}  // namespace aegir::terminal