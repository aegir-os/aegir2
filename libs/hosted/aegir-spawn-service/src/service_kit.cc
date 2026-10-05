/*
 * The spawn service's kit, adopted (specs/authority.md, specs/shell.md,
 * specs/memory.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The machinery of service_kit.h: the staged command allocator whose untyped
 * source is mem.main, the reserved slot pool one owner per live command, and
 * the reap that returns both. The grants the children get are built elsewhere
 * (aegir/spawn/kit.h); this owns the memory and slots a spawn runs on.
 */

#include <aegir/spawn/service_kit.h>

#include <aegir/bootstrap.h>
#include <aegir/console_stream.h>
#include <aegir/datatypes.h>
#include <aegir/debug.h>
#include <aegir/environment.h>
#include <aegir/log.h>
#include <aegir/memory.h>
#include <aegir/mem/vspace.h>
#include <aegir/net.h>
#include <aegir/nmspace.h>

#include <unistd.h>

namespace aegir::spawn {

namespace {

/* The allocator over the command chunks: static, like auth's session
 * allocator, because its node table is far larger than a stack. Its untyped
 * source is mem.main (specs/memory.md); it is reset for each command's
 * staging, because a command's chunks are its own and the records for them
 * are not wanted past the spawn. */
aegir::mem::Allocator g_command_mem(nullptr);

/* The command allocator's node pool: the toolkit's window wires its own
 * allocator's pool, so a second allocator brings its own region. A command's
 * spawn splits a few hundred pieces; a nested terminal's image is far larger
 * (its segments near a megabyte, so hundreds of frames), so this is sized for
 * that, not a command's handful. Reset() rebuilds it each spawn, so it never
 * accumulates. */
alignas(64) unsigned char g_command_nodes[256 * 1024];

/* The current command's badged mem.main copy, which the untyped source calls
 * with. It is set by begin_command and cleared by end_staging. */
seL4_CPtr g_command_mem_call = 0;

/* The allocator's untyped source (specs/memory.md): ask mem.main for a chunk
 * through the current command's badged copy, so every chunk the process
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
                          endpoint_depth());
        return 0;
    }
    *size_bits = static_cast<seL4_Word>(reply.count >= 1 ? answer[0]
                                                         : aegir::memory::kChunkBits);
    *paddr = 0;
    return slot;
}

}  // namespace

bool ServiceKit::adopt(aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch,
                       uint64_t slot_base, uint32_t slot_count, bool slot_descend)
{
    if (ready_) {
        return true;
    }
    allocator_ = &allocator;
    scratch_ = &scratch;
    slot_base_ = slot_base;
    slot_count_ = slot_count;
    slot_descend_ = slot_descend;

    /* The memory service, unbadged: a command's own copy is minted from it
     * (specs/memory.md). */
    uint64_t mem_slot = 0;
    if (!aegir::bootstrap::capability("spawn:mem.main", 14, &mem_slot)) {
        return false;
    }
    mem_port_ = static_cast<seL4_CPtr>(mem_slot);

    /* The shell's pool: a second pool, because a shell is spawned once and
     * never reclaimed (specs/shell.md). Optional: the session's launcher draws
     * a nested shell's pool from mem.main and is given none. */
    uint64_t shell_pool_slot = 0;
    uint32_t shell_pool_bits = 0;
    if (aegir::bootstrap::capability("shell-pool", 10, &shell_pool_slot) &&
        aegir::bootstrap::capability_size_bits("shell-pool", 10, &shell_pool_bits)) {
        shell_pool_ = static_cast<seL4_CPtr>(shell_pool_slot);
        shell_pool_bits_ = shell_pool_bits;
    }

    uint64_t asid_pool = 0;
    if (!aegir::bootstrap::capability("asid-pool", 9, &asid_pool)) {
        return false;
    }
    asid_pool_ = static_cast<seL4_CPtr>(asid_pool);

    /* The unbadged log copy a command's own cap is minted from, and the
     * shell's namespace, badged with this process's own badge so the shell
     * resolves the session's aliases (specs/shell.md). */
    uint64_t log_slot = 0;
    uint64_t nmspace_slot = 0;
    if (!aegir::bootstrap::capability("spawn:log.main", 14, &log_slot) ||
        !aegir::bootstrap::capability("shell:vfs.namespace", 19, &nmspace_slot)) {
        return false;
    }
    log_port_ = static_cast<seL4_CPtr>(log_slot);
    nmspace_port_ = static_cast<seL4_CPtr>(nmspace_slot);

    /* The process's own badged namespace, handed to a command by *copy* so it
     * carries the session's identity (specs/dos.md). The namespace name is the
     * runtime's own find, so the process resolves exactly what a command
     * will. */
    uint64_t command_nmspace_slot = 0;
    if (!aegir::bootstrap::capability(aegir::nmspace::kPortName,
                                      aegir::nmspace::kPortNameLength,
                                      &command_nmspace_slot)) {
        return false;
    }
    command_nmspace_port_ = static_cast<seL4_CPtr>(command_nmspace_slot);

    /* The clock, when one was delegated: unbadged, so a fresh copy can be
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
    /* The network's sources (specs/net.md): the terminal mints each command a
     * caller half of the socket and control ports from them. Optional. */
    seL4_CPtr command_net_socket = 0;
    uint64_t command_net_socket_slot = 0;
    if (aegir::bootstrap::capability("net.socket", 10, &command_net_socket_slot)) {
        command_net_socket = static_cast<seL4_CPtr>(command_net_socket_slot);
    }
    seL4_CPtr command_net_control = 0;
    uint64_t command_net_control_slot = 0;
    if (aegir::bootstrap::capability("net.control", 11, &command_net_control_slot)) {
        command_net_control = static_cast<seL4_CPtr>(command_net_control_slot);
    }

    /* The boot session's status endpoint (specs/boot.md): auth grants it only
     * to the boot terminal, which passes it on to the shell. Its presence is
     * what makes this process the boot session's. */
    uint64_t boot_status_slot = 0;
    if (aegir::bootstrap::capability("boot.status", 11, &boot_status_slot)) {
        boot_status_ = static_cast<seL4_CPtr>(boot_status_slot);
    }

    /* Our own con.stream endpoint and a fault endpoint for our children,
     * retyped from the process's memory (they live as long as the process, not
     * as long as a command). A launcher draws no stream of its own and only
     * uses the fault endpoint; the stream cap is left for it to set per
     * request. */
    seL4_Error error = seL4_NoError;
    stream_endpoint_ = allocator_->alloc_object(seL4_EndpointObject,
                                                seL4_EndpointBits, account_, &error);
    fault_endpoint_ = allocator_->alloc_object(seL4_EndpointObject,
                                               seL4_EndpointBits, account_, &error);
    if (stream_endpoint_ == 0 || fault_endpoint_ == 0) {
        return false;
    }
    g_command_mem.adopt_nodes(g_command_nodes, sizeof(g_command_nodes));

    /* A two-level CSpace gives each command its own L2 CNode; a single-level one
     * shares the reserved pool among commands (specs/memory.md). */
    cnode_l1_ = aegir::bootstrap::cnode_l1_bits();
    cnode_l2_ = aegir::bootstrap::cnode_bits();
    if (cnode_l1_ != 0) {
        /* Root slot 0 holds the L2 CNode with our own caps; the pool's CNodes
         * start at 1. No shared slot table: a command's whole CNode comes and
         * goes with it. */
        next_l1_slot_ = 1;
        g_command_mem.adopt_slot_pool(nullptr, 0);
    } else {
        /* The command-slot pool: the reserved spawn range, one owner per live
         * command (specs/memory.md Phase 5). The allocator's node pool is reset
         * per command, but this pool is not: the slots a live command still
         * holds stay marked. */
        slot_owners_.assign(slot_count_, 0);
        slot_pool_.adopt(slot_base_, slot_count_, slot_owners_.data(), slot_descend_);
        /* A service addresses its own slots at depth zero (the node itself),
         * and the allocator has to be told (adopt_slots explains); there is no
         * cursor because the pool is the slot source. */
        g_command_mem.adopt_slots(0, 0, 0, aegir::bootstrap::cnode_bits());
        g_command_mem.adopt_slot_pool(&slot_pool_, 0);
    }
    g_command_mem.set_untyped_source(command_untyped_source, &g_command_mem);

    /* The launcher kit (specs/launch.md): the unbadged console.gui a nested
     * terminal's own is minted from. Its memory comes from mem.main on demand,
     * so there is no pool here to size. Optional -- a process auth delegated no
     * launcher kit still runs and refuses a launching launch. */
    uint64_t spawn_gui_slot = 0;
    seL4_CPtr spawn_console_gui = 0;
    if (aegir::bootstrap::capability("spawn:console.gui", 17, &spawn_gui_slot)) {
        spawn_console_gui = static_cast<seL4_CPtr>(spawn_gui_slot);
    }

    /* The bureau.menu source (specs/workbench.md): the launcher mints each of
     * its commands a caller half from it, so a launched program can register
     * the menus the screen bar shows while it is active. Optional -- a launcher
     * given none hands its commands none, and they run without menus. */
    uint64_t bureau_menu_slot = 0;
    seL4_CPtr bureau_menu_holder = 0;
    if (aegir::bootstrap::capability("bureau.menu", 11, &bureau_menu_slot)) {
        bureau_menu_holder = static_cast<seL4_CPtr>(bureau_menu_slot);
    }

    /* The unbadged font.main source (specs/fonts.md): the launcher mints each
     * command a caller half from it, so a launched program draws a Sys:Fonts
     * face the toolkit cannot parse itself. Optional. */
    uint64_t font_main_slot = 0;
    seL4_CPtr font_main_holder = 0;
    if (aegir::bootstrap::capability("font.main", 9, &font_main_slot)) {
        font_main_holder = static_cast<seL4_CPtr>(font_main_slot);
    }

    /* The unbadged datatypes.main source (specs/datatypes.md): the session's
     * broker, so the launcher mints each command a caller half from it and a
     * launched program asks the broker to open a file. Optional -- without it a
     * command starts its class directly through the launcher. */
    uint64_t datatypes_slot = 0;
    seL4_CPtr datatypes_holder = 0;
    if (aegir::bootstrap::capability(aegir::datatypes::kBrokerPortName,
                                     aegir::datatypes::kBrokerPortNameLength,
                                     &datatypes_slot)) {
        datatypes_holder = static_cast<seL4_CPtr>(datatypes_slot);
    }

    /* The launch caller half (specs/launch.md): the launcher's own copy, the
     * `spawn:launch.session` auth granted it, so it can hand a nested terminal
     * the same caller half a shell gets; a terminal that owns no launcher reads
     * the `launch.session` auth granted it instead -- which is the boot
     * session's terminal now that the boot session has a launcher of its own,
     * so a Startup-Sequence line starts a command. Optional: a process with
     * neither (a degraded boot's terminal) grants its shell none. */
    uint64_t launch_slot = 0;
    seL4_CPtr launch_holder = 0;
    if (aegir::bootstrap::capability("spawn:launch.session", 20, &launch_slot) ||
        aegir::bootstrap::capability(aegir::launch::kPortName,
                                     aegir::launch::kPortNameLength, &launch_slot)) {
        launch_holder = static_cast<seL4_CPtr>(launch_slot);
    }

    /* The spawner insists on an Initrd it never reads when an image is given:
     * the shell hands the one command's bytes as `binary_image`
     * (specs/shell.md). */
    initrd_ = std::make_unique<aegir::spawn::Initrd>(nullptr, 0);
    /* The kit every child's grant is built from (specs/launch.md): the
     * delegates above and the endpoints made here, in the one struct the
     * builders read. */
    kit_.log = log_port_;
    kit_.console_gui = spawn_console_gui;
    kit_.bureau_menu = bureau_menu_holder;
    kit_.font_main = font_main_holder;
    kit_.datatypes = datatypes_holder;
    kit_.mem_main = mem_port_;
    kit_.asid_pool = asid_pool_;
    kit_.clock = command_clock_port_;
    kit_.timer = command_timer_port_;
    kit_.net_socket = command_net_socket;
    kit_.net_control = command_net_control;
    kit_.nmspace = command_nmspace_port_;
    kit_.shell_nmspace = nmspace_port_;
    kit_.stream = stream_endpoint_;
    kit_.launch = launch_holder;
    kit_.boot_status = boot_status_;
    ready_ = true;
    return true;
}

aegir::mem::Allocator& ServiceKit::memory()
{
    return g_command_mem;
}

bool ServiceKit::spawn_shell(char const *image, uint64_t image_bytes, char const *cwd,
                             uint32_t cwd_length, uint64_t badge,
                             char const *const *arguments, uint32_t argument_count)
{
    if (!ready_ || shell_pool_ == 0 || allocator_ == nullptr) {
        aegir::debug_write("  spawn: shell spawn: not ready\n");
        return false;
    }
    /* The shell's pool is its own runtime untyped: dedicated, so it is handed
     * over whole rather than carved, and the shell's heap grows into it. Its
     * objects and page tables come from the process's allocator, which is the
     * one the process already owns. */
    aegir::mem::Account account{"shell", 0, 0, 0};
    aegir::mem::Arena arena(*allocator_, *scratch_, account);
    aegir::spawn::Initrd const initrd(nullptr, 0);
    aegir::spawn::Spawner spawner(*allocator_, *scratch_, arena, initrd, asid_pool_,
                                  static_cast<seL4_CPtr>(aegir::bootstrap::kSlotOwnCNode),
                                  endpoint_depth());
    aegir::spawn::Child child{};
    child.badge = badge;
    child.runtime = shell_pool_;
    child.runtime_bits = shell_pool_bits_;
    aegir::spawn::PortGrant ports[6];
    uint32_t port_count = aegir::spawn::shell_ports(kit_, child, ports, 6);
    /* The boot session's status endpoint (specs/boot.md): the shell sends the
     * outcome here and auth receives it. Only the boot process has one, and it
     * is not part of the kit a launched program gets, so it is appended. */
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
    /* The process's own environment rides to the shell: the boot flags auth set
     * as AEGIR_BOOTARGS are how the shell learns to force the failure view
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
    /* The pool's physical is not known to us, and only a driver needs it; zero
     * is the "not given" the block and the allocator accept. */
    request.untyped_physical = 0;
    request.untyped_bits = shell_pool_bits_;
    aegir::spawn::Process process{};
    if (!spawner.spawn(request, account, process)) {
        aegir::debug_write("  spawn: shell spawn: ");
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

bool ServiceKit::begin(uint32_t owner)
{
    if (!ready_) {
        return false;
    }
    /* The allocator is reset per command: a command's chunks are its own, and
     * its objects are all retyped before staging ends. The slot source is not
     * reset the same way: a two-level process gives the command its own L2
     * CNode, and a single-level one keeps live commands' slots marked in the
     * shared pool. */
    g_command_mem.reset();
    if (cnode_l1_ != 0) {
        seL4_Word slot = 0;
        for (OwnerCnode const &entry : owner_cnodes_) {
            if (entry.owner == owner) {
                slot = entry.slot;
                break;
            }
        }
        if (slot == 0) {
            if (!free_l1_slots_.empty()) {
                slot = free_l1_slots_.back();
                free_l1_slots_.pop_back();
            } else if (next_l1_slot_ < (static_cast<seL4_Word>(1) << cnode_l1_)) {
                slot = next_l1_slot_++;
            } else {
                return false;
            }
            seL4_Error error = seL4_NoError;
            void *cookie = nullptr;
            seL4_CPtr const created = allocator_->alloc_cnode_at_l1(
                cnode_l2_, slot, account_, &error, &cookie);
            if (created == 0) {
                free_l1_slots_.push_back(slot);
                return false;
            }
            owner_cnodes_.push_back(OwnerCnode{owner, slot, cookie});
        }
        g_command_mem.adopt_slots_level_two(0, static_cast<seL4_Word>(1) << cnode_l2_,
                                            cnode_l1_, cnode_l2_, slot);
    } else {
        g_command_mem.adopt_slot_pool(&slot_pool_, owner);
    }
    g_command_mem.set_untyped_source(command_untyped_source, &g_command_mem);
    g_command_mem_call = 0;
    /* The staging mark is the window position before the *first* still-live
     * command was staged; rewind_staging returns there once they are all gone
     * (specs/auth.md's reclaim shape, per command). */
    if (!staged_since_rewind_) {
        scratch_mark_ = scratch_->next();
        staged_since_rewind_ = true;
    }
    /* The install source is a slot in this process's own CSpace: a plain number
     * in a single-level one, and an L2 address in a two-level one (a level's
     * bits are just its radix, guards zero). */
    seL4_Word const source_depth =
        cnode_l1_ != 0 ? cnode_l1_ + cnode_l2_ : aegir::bootstrap::cnode_bits();
    arena_ = std::make_unique<aegir::mem::Arena>(g_command_mem, *scratch_, account_);
    spawner_ = std::make_unique<aegir::spawn::Spawner>(
        g_command_mem, *scratch_, *arena_, *initrd_, asid_pool_,
        static_cast<seL4_CPtr>(aegir::bootstrap::kSlotOwnCNode), source_depth);
    return true;
}

bool ServiceKit::begin_command(uint64_t badge)
{
    if (!ready_ || mem_port_ == 0 || allocator_ == nullptr) {
        return false;
    }
    /* The command's memory copy: minted from the unbadged service port with
     * the command's own id, so the service records every chunk retyped for it
     * as that command's (specs/memory.md). A copy of it goes to the command,
     * so its own runtime grows within the same ownership. The slot is reused:
     * end_staging drops the previous copy. */
    if (command_mem_ == 0) {
        command_mem_ = allocator_->alloc_slot();
        if (command_mem_ == 0) {
            return false;
        }
        command_mem_live_ = false;
    }
    if (command_mem_live_) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, command_mem_,
                          endpoint_depth());
        command_mem_live_ = false;
    }
    seL4_Error const minted = seL4_CNode_Mint(
        aegir::bootstrap::kSlotOwnCNode, command_mem_, endpoint_depth(),
        aegir::bootstrap::kSlotOwnCNode, mem_port_, endpoint_depth(),
        seL4_CapRights_new(1, 1, 0, 1), badge);
    if (minted != seL4_NoError) {
        return false;
    }
    command_mem_live_ = true;
    g_command_mem_call = command_mem_;
    return true;
}

void ServiceKit::end_staging()
{
    spawner_.reset();
    arena_.reset();
    g_command_mem_call = 0;
    /* The image's segments are in the child now, so the file the spawner read
     * them from is done: close it, or the next load would leave it open behind
     * (load_image closes a stale one too, belt and braces). */
    if (image_fd_ >= 0) {
        ::close(image_fd_);
        image_fd_ = -1;
    }
    /* The command holds its own copy of the badged mem.main port; ours is a
     * staging cap and goes now. The window is not rewound: the staging frames
     * are the command's memory until it is reaped, and only rewind_staging
     * returns the window once they are gone. */
    if (command_mem_live_) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, command_mem_,
                          endpoint_depth());
        command_mem_live_ = false;
    }
}

void ServiceKit::abandon(uint64_t badge, uint32_t owner)
{
    end_staging();
    reap(0, badge, owner);
}

void ServiceKit::rewind_staging()
{
    if (!staged_since_rewind_) {
        return;
    }
    scratch_->rewind(scratch_mark_);
    staged_since_rewind_ = false;
}

void ServiceKit::reap(seL4_CPtr tcb, uint64_t badge, uint32_t owner)
{
    /* Stop the command before its capabilities go: a running thread whose TCB
     * is revoked is undefined. It has already halted on its own; the suspend
     * is what makes that certain. */
    if (tcb != 0) {
        seL4_TCB_Suspend(tcb);
    }
    /* The memory's way back is one release per command: the service revokes
     * every chunk the command owned, and its TCB, page tables and frames --
     * retyped from those chunks -- go with them (specs/memory.md). The slots
     * are empty afterwards, so the owner's range returns whole. */
    if (mem_port_ != 0) {
        aegir::ipc::Consumer const service(mem_port_);
        uint64_t const word = badge;
        uint64_t released = 0;
        (void)service.call_words(aegir::memory::kMethodRelease, &word, 1, &released, 1);
    }
    /* The client's sockets are the stack's, not its memory: a client that died
     * without closing leaves them in the stack's heap, where seL4 cannot reach
     * them. The launcher already holds the socket port -- it mints each command
     * its caller half from it -- so it names the badge and the stack drops what
     * that badge owned (aegir/net.h's kMethodReap). A process given no socket
     * port has nothing to reap. */
    if (kit_.net_socket != 0) {
        aegir::ipc::Consumer const stack(kit_.net_socket);
        uint64_t const word = badge;
        uint64_t dropped = 0;
        (void)stack.call_words(aegir::net::kMethodReap, &word, 1, &dropped, 1);
    }
    if (cnode_l1_ != 0) {
        for (std::size_t i = 0; i < owner_cnodes_.size(); ++i) {
            if (owner_cnodes_[i].owner != owner) {
                continue;
            }
            /* Give the L2 CNode back: freeing the piece revokes the CNode cap
             * at its root slot and returns the memory, so the slot is ready for
             * the next command (specs/memory.md). */
            if (owner_cnodes_[i].cookie != nullptr) {
                (void)allocator_->free_object(
                    owner_cnodes_[i].cookie,
                    static_cast<seL4_Word>(aegir::mem::Allocator::object_bits(
                        seL4_CapTableObject, cnode_l2_)));
            }
            free_l1_slots_.push_back(owner_cnodes_[i].slot);
            owner_cnodes_.erase(owner_cnodes_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    } else {
        slot_pool_.free_owner(owner);
    }
}

bool ServiceKit::hold_received_stream()
{
    if (allocator_ == nullptr) {
        return false;
    }
    if (stream_slot_ == 0) {
        stream_slot_ = allocator_->alloc_slot();
        if (stream_slot_ == 0) {
            return false;
        }
        stream_live_ = false;
    }
    if (stream_live_) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, stream_slot_,
                          endpoint_depth());
        stream_live_ = false;
    }
    if (!aegir::ipc::take_received_cap(stream_slot_)) {
        return false;
    }
    stream_live_ = true;
    return true;
}

}  // namespace aegir::spawn
