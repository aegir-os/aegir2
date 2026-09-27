/*
 * The terminal's spawn kit, adopted (specs/authority.md, specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "spawn_kit.h"

#include <aegir/bootstrap.h>
#include <aegir/console_stream.h>
#include <aegir/debug.h>
#include <aegir/environment.h>
#include <aegir/log.h>
#include <aegir/memory.h>
#include <aegir/mem/vspace.h>
#include <aegir/nmspace.h>
#include <aegir/trinket/application.h>

namespace aegir::terminal {

namespace {

/* The allocator over the command chunks: static, like auth's session
 * allocator, because its node table is far larger than a stack. It is reset
 * for each bracket (begin); its untyped source is mem.main. */
aegir::mem::Allocator g_command_mem(nullptr);

/* The command allocator's node pool: the toolkit's window wires its own
 * allocator's pool, so a second allocator brings its own region. A command's
 * spawn splits a few hundred pieces; 64 KiB is beyond that and lives in BSS. */
alignas(64) unsigned char g_command_nodes[64 * 1024];

/* The current command's badged mem.main copy, which the untyped source calls
 * with. It is set by begin_command and cleared by reclaim. */
seL4_CPtr g_command_mem_call = 0;

/* The allocator's untyped source (specs/memory.md): ask mem.main for a chunk
 * through the current command's badged copy, so every chunk the terminal
 * retypes the command from is owned by the command's id and comes back in one
 * release. The chunk rides the reply into the scratch receive slot and is
 * moved into a slot of the allocator's own, because the spawner must keep
 * retyping from it. */
seL4_CPtr command_untyped_source(void *context, seL4_Word *size_bits,
                                 uint64_t *paddr) noexcept
{
    auto *const allocator = static_cast<aegir::mem::Allocator *>(context);
    if (allocator == nullptr || g_command_mem_call == 0) {
        return 0;
    }
    aegir::ipc::Consumer const service(g_command_mem_call);
    uint64_t const request = aegir::memory::kChunkBits;
    uint64_t answer[1] = {};
    bool cap_arrived = false;
    aegir::ipc::WordsReply const reply = service.call_transfer(
        aegir::memory::kMethodAlloc, &request, 1, 0, answer, 1, &cap_arrived);
    if (reply.error != 0 || !cap_arrived) {
        return 0;
    }
    seL4_CPtr const slot = allocator->alloc_slot();
    if (slot == 0 || !aegir::ipc::take_received_cap(slot)) {
        /* The chunk is still in the scratch receive slot; drop it, or the next
         * transfer is refused an occupied slot. */
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                          aegir::bootstrap::kSlotReceiveCap,
                          aegir::bootstrap::kCNodeBits);
        return 0;
    }
    *size_bits = static_cast<seL4_Word>(reply.count >= 1 ? answer[0]
                                                         : aegir::memory::kChunkBits);
    *paddr = 0;
    return slot;
}

}  // namespace

bool SpawnKit::adopt(aegir::trinket::Application& app)
{
    if (ready_) {
        return true;
    }
    app_ = &app;

    /* The memory service, unbadged: a command's own copy is minted from it
     * (specs/memory.md Phase 3). */
    uint64_t mem_slot = 0;
    if (!aegir::bootstrap::capability("spawn:mem.main", 14, &mem_slot)) {
        return false;
    }
    mem_port_ = static_cast<seL4_CPtr>(mem_slot);

    /* The shell's pool: a second pool, because the shell is spawned once and
     * never reclaimed (specs/shell.md). */
    uint64_t shell_pool_slot = 0;
    uint32_t shell_pool_bits = 0;
    if (!aegir::bootstrap::capability("shell-pool", 10, &shell_pool_slot) ||
        !aegir::bootstrap::capability_size_bits("shell-pool", 10, &shell_pool_bits)) {
        return false;
    }
    shell_pool_ = static_cast<seL4_CPtr>(shell_pool_slot);
    shell_pool_bits_ = shell_pool_bits;

    uint64_t asid_pool = 0;
    if (!aegir::bootstrap::capability("asid-pool", 9, &asid_pool)) {
        return false;
    }
    asid_pool_ = static_cast<seL4_CPtr>(asid_pool);

    /* The unbadged log copy a command's own cap is minted from, and the
     * shell's namespace, badged with the terminal's own badge so the shell
     * resolves the session's aliases (specs/shell.md). */
    uint64_t log_slot = 0;
    uint64_t nmspace_slot = 0;
    if (!aegir::bootstrap::capability("spawn:log.main", 14, &log_slot) ||
        !aegir::bootstrap::capability("shell:vfs.namespace", 19, &nmspace_slot)) {
        return false;
    }
    log_port_ = static_cast<seL4_CPtr>(log_slot);
    nmspace_port_ = static_cast<seL4_CPtr>(nmspace_slot);

    /* The terminal's own badged namespace, handed to a command by *copy* so it
     * carries the session's identity (specs/dos.md). The namespace name is the
     * runtime's own find, so the terminal resolves exactly what a command
     * will. */
    uint64_t command_nmspace_slot = 0;
    if (!aegir::bootstrap::capability(aegir::nmspace::kPortName,
                                      aegir::nmspace::kPortNameLength,
                                      &command_nmspace_slot)) {
        return false;
    }
    command_nmspace_port_ = static_cast<seL4_CPtr>(command_nmspace_slot);

    /* The clock, when auth was given one: unbadged, so a fresh copy can be
     * minted for the shell and each command (specs/dos.md). Optional -- a
     * session without a clock still runs, and only the time tools report it. */
    uint64_t command_clock_slot = 0;
    if (aegir::bootstrap::capability("spawn:clock.main", 16, &command_clock_slot)) {
        command_clock_port_ = static_cast<seL4_CPtr>(command_clock_slot);
    }
    /* The timer, the interval side (specs/timer.md): optional the same way. */
    uint64_t command_timer_slot = 0;
    if (aegir::bootstrap::capability("spawn:timer.main", 16, &command_timer_slot)) {
        command_timer_port_ = static_cast<seL4_CPtr>(command_timer_slot);
    }

    /* The boot session's doorbell (specs/boot.md): auth grants it only to the
     * boot terminal, which passes it on to the shell. Its presence is what
     * makes this terminal the boot session's. */
    uint64_t boot_status_slot = 0;
    if (aegir::bootstrap::capability("boot.status", 11, &boot_status_slot)) {
        boot_status_ = static_cast<seL4_CPtr>(boot_status_slot);
    }

    /* The terminal's own con.stream endpoint and a fault endpoint for its
     * children, retyped from the toolkit's memory (they live as long as the
     * terminal, not as long as a command). */
    seL4_Error error = seL4_NoError;
    stream_endpoint_ = app.allocator().alloc_object(seL4_EndpointObject,
                                                    seL4_EndpointBits, account_, &error);
    fault_endpoint_ = app.allocator().alloc_object(seL4_EndpointObject,
                                                   seL4_EndpointBits, account_, &error);
    /* The command doorbell: the terminal rings it when a running command's
     * stream has input, so a command's `read` parks on it (specs/terminal.md).
     * Long-lived, like the endpoints: one notification for the one command
     * that runs at a time. */
    command_doorbell_ = app.allocator().alloc_object(seL4_NotificationObject,
                                                     seL4_NotificationBits, account_, &error);
    if (stream_endpoint_ == 0 || fault_endpoint_ == 0 || command_doorbell_ == 0) {
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

bool SpawnKit::spawn_shell(char const *image, uint64_t image_bytes, char const *cwd,
                           uint32_t cwd_length, uint64_t badge,
                           char const *const *arguments, uint32_t argument_count)
{
    if (!ready_ || shell_pool_ == 0 || app_ == nullptr) {
        aegir::debug_write("  terminal: shell spawn: not ready\n");
        return false;
    }
    /* The shell's pool is its own runtime untyped: dedicated, so it is handed
     * over whole rather than carved, and the shell's heap grows into it. Its
     * objects and page tables come from the toolkit's allocator, which is the
     * one the terminal already owns. */
    aegir::mem::Account account{"shell", 0, 0, 0};
    aegir::mem::Arena arena(app_->allocator(), app_->scratch(), account);
    aegir::spawn::Initrd const initrd(nullptr, 0);
    aegir::spawn::Spawner spawner(app_->allocator(), app_->scratch(), arena, initrd,
                                  asid_pool_,
                                  static_cast<seL4_CPtr>(aegir::bootstrap::kSlotOwnCNode),
                                  aegir::bootstrap::kCNodeBits);
    aegir::spawn::PortGrant ports[5] = {
        {aegir::console::kStreamPortName, aegir::console::kStreamPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared, stream_endpoint_,
         seL4_CapRights_new(1, 1, 0, 1), badge, 0},
        {aegir::log::kPortName, aegir::log::kPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared + 1, log_port_,
         seL4_CapRights_new(1, 0, 0, 1), 0, 0},
        /* The shell's namespace is moved, not minted: it already carries the
         * terminal's badge, and a badged cap cannot be minted again. The shell
         * is spawned once, so the one move is the one use (specs/shell.md). */
        {aegir::nmspace::kPortName, aegir::nmspace::kPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared + 2, nmspace_port_,
         seL4_CapRights_new(1, 1, 0, 1), 0, 0, true},
        {"untyped", 7, aegir::bootstrap::kSlotFirstDeclared + 3, shell_pool_,
         seL4_AllRights, 0, shell_pool_bits_},
    };
    uint32_t port_count = 4;
    /* The boot session's status endpoint (specs/boot.md): the shell sends the
     * outcome here and auth receives it. Only the boot terminal has one. */
    if (boot_status_ != 0) {
        ports[port_count] = {"boot.status", 11,
                             aegir::bootstrap::kSlotFirstDeclared + port_count,
                             boot_status_, seL4_AllRights, 0, 0};
        ++port_count;
    }
    static char const kName[] = "session.shell";
    static char const kAccountText[] = "shell";
    aegir::spawn::Request request{};
    request.name = kName;
    request.name_length = sizeof(kName) - 1;
    request.binary_image = image;
    request.binary_image_bytes = image_bytes;
    request.account = kAccountText;
    request.account_length = sizeof(kAccountText) - 1;
    request.cwd = cwd;
    request.cwd_length = cwd_length;
    /* For the boot session, the command file the shell runs (specs/boot.md);
     * an interactive shell is started with none. */
    request.arguments = arguments;
    request.argument_count = argument_count;
    /* The terminal's own environment rides to the shell: the boot flags auth
     * set as AEGIR_BOOTARGS are how the shell learns to force the failure view
     * (specs/boot.md). */
    request.environment = aegir::environment::environ();
    request.environment_count = 0;
    while (request.environment[request.environment_count] != nullptr) {
        ++request.environment_count;
    }
    request.priority = seL4_MaxPrio - 2;
    request.ports = ports;
    request.port_count = port_count;
    request.fault_endpoint = fault_endpoint_;
    request.badge = badge;
    request.give_vspace = true;
    /* The pool's physical is not known to the terminal, and only a driver
     * needs it; zero is the "not given" the block and the allocator accept. */
    request.untyped_physical = 0;
    request.untyped_bits = shell_pool_bits_;
    aegir::spawn::Process process{};
    if (!spawner.spawn(request, account, process)) {
        aegir::debug_write("  terminal: shell spawn: ");
        aegir::debug_write(spawner.problem());
        char const *const detail = spawner.detail();
        if (detail != nullptr && detail[0] != '\0') {
            aegir::debug_write(" (");
            aegir::debug_write(detail);
            aegir::debug_write(")");
        }
        aegir::debug_write("\n");
        return false;
    }
    return true;
}

bool SpawnKit::begin()
{
    if (!ready_) {
        return false;
    }
    /* No pool to adopt: the command chunks come from mem.main as the spawner
     * asks for them (specs/memory.md Phase 3). The source is this allocator's
     * once, in adopt(). */
    g_command_mem.reset();
    g_command_mem.adopt_slots(app_->spawn_slot_base(), app_->spawn_slot_count(), 0);
    g_command_mem.set_untyped_source(command_untyped_source, &g_command_mem);
    command_badges_.clear();
    g_command_mem_call = 0;
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

bool SpawnKit::begin_command(uint64_t badge)
{
    if (!ready_ || mem_port_ == 0 || app_ == nullptr) {
        return false;
    }
    /* The command's memory copy: minted from the unbadged service port with
     * the command's own id, so the service records every chunk the terminal
     * retypes for it as that command's (specs/memory.md Phase 3). A copy of it
     * goes to the command, so its own runtime grows within the same ownership.
     * The slot is reused: the previous copy is dropped first. */
    if (command_mem_ == 0) {
        command_mem_ = app_->allocator().alloc_slot();
        if (command_mem_ == 0) {
            return false;
        }
    }
    if (command_mem_live_) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, command_mem_,
                          aegir::bootstrap::kCNodeBits);
        command_mem_live_ = false;
    }
    seL4_Error const minted = seL4_CNode_Mint(
        aegir::bootstrap::kSlotOwnCNode, command_mem_, aegir::bootstrap::kCNodeBits,
        aegir::bootstrap::kSlotOwnCNode, mem_port_, aegir::bootstrap::kCNodeBits,
        seL4_CapRights_new(1, 1, 0, 1), badge);
    if (minted != seL4_NoError) {
        return false;
    }
    command_mem_live_ = true;
    g_command_mem_call = command_mem_;
    command_badges_.push_back(badge);
    return true;
}

void SpawnKit::reclaim()
{
    spawner_.reset();
    arena_.reset();
    /* The memory's way back is one release per command: the service revokes
     * every chunk the command owned, and the command's TCB, page tables and
     * frames -- retyped from those chunks -- go with them (specs/memory.md
     * Phase 3). The reserved slots are empty afterwards, so the cursor returns
     * to the base. */
    g_command_mem_call = 0;
    if (mem_port_ != 0) {
        aegir::ipc::Consumer const service(mem_port_);
        for (uint64_t badge : command_badges_) {
            uint64_t const word = badge;
            uint64_t released = 0;
            (void)service.call_words(aegir::memory::kMethodRelease, &word, 1, &released, 1);
        }
    }
    command_badges_.clear();
    g_command_mem.slot_release(app_->spawn_slot_base());
    app_->scratch().rewind(scratch_mark_);
}

void SpawnKit::finish_all(std::vector<aegir::spawn::Process> const &processes)
{
    /* Stop each command before its capabilities go: a running thread whose
     * TCB is revoked is undefined. The commands have already halted on their
     * own; the suspend is what makes that certain. */
    for (aegir::spawn::Process const &process : processes) {
        if (process.tcb != 0) {
            seL4_TCB_Suspend(process.tcb);
        }
    }
    reclaim();
}

void SpawnKit::abort()
{
    reclaim();
}

}  // namespace aegir::terminal