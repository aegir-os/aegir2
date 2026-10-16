/*
 * The supervisor thread -- implementation. See src/supervisor.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "supervisor.h"

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/memory.h>
#include <sel4/faults.h>

/* sel4runtime's TLS helpers, declared here rather than by including its header:
 * sel4runtime.h is C-only (specs/userland.md), and these are the three things a
 * thread needs that a process gets from its crt. */
extern "C" {
uintptr_t sel4runtime_get_tls_size(void);
uintptr_t sel4runtime_write_tls_image(void *tls_memory);
void __sel4runtime_write_tls_variable(uintptr_t thread_pointer, unsigned char *variable,
                                      unsigned char *value, uint64_t size);
/* Where libsel4 keeps the IPC buffer pointer: per *thread*, in TLS. Every syscall
 * wrapper reads it, so a thread whose thread pointer is zero faults on its first
 * syscall -- which is exactly what happened here. It is declared by libsel4
 * itself (kernel/libsel4/include/sel4/functions.h:13), so it is used, not
 * redeclared. */
}

namespace aegir::director {

namespace {

constexpr uint64_t kPage = 1ull << seL4_PageBits;

/* The pager's slot pool (specs/pager.md): a reserved part of director's free
 * CSpace, owned by the supervisor thread alone, so it never touches the
 * allocator the boot thread spawns with. Frames and page tables are retyped
 * from a child-badged mem.main chunk into these plain root slots. */
aegir::mem::Allocator g_pager_mem(nullptr);
alignas(64) unsigned char g_pager_nodes[16 * 1024];
aegir::mem::Account g_pager_account{"director.pager", 0, 0, 0};

/* What the pager's frame source needs: the supervisor and the badge, so the
 * record can be re-found by badge. The table is published before any child
 * runs, so the supervisor only reads it. */
struct PagerContext {
    Supervisor *self;
    uint64_t badge;
};

/* What the thread needs to see. A thread cannot be started with arguments -- its
 * program counter and stack pointer are all it gets -- so the world it works in is
 * a global, filled before it starts. */
struct World {
    seL4_CPtr endpoint;
    Supervised *table;
    uint32_t capacity;
    uint32_t *count;
    unsigned *faults;
    Supervisor *self;
};

World g_world;

void write(char const *text) noexcept
{
    aegir::debug_write(text);
}

void write_word(uint64_t value) noexcept
{
    aegir::debug_write_unsigned(value);
}

/* Through the line buffer, never straight to the serial. seL4_DebugPutChar
 * lands a character *now*, while the line it belongs to is still being
 * assembled; a name written that way turned up in front of its own line and
 * left the parentheses empty -- "hello  supervisor: service 24 () faulted".
 * aegir::debug_write puts it in the buffer with the rest of the line. */
void write_name(char const *name, uint32_t length) noexcept
{
    aegir::debug_write(name, length);
}

/** What went wrong, said by the thing whose job it is to notice
 *  (specs/director.md). The port the logger owns is where this belongs; the
 *  console carries it until director has a capability to that port of its own.
 *  `pc`/`addr` are read from the message registers by the caller, before the
 *  pager's mem.main call can replace them (specs/pager.md). */
void report_fault(seL4_Word badge, seL4_MessageInfo_t info, uint64_t pc, uint64_t addr,
                  spawn::StackFault const *overflow, char const *why) noexcept
{
    write("  supervisor: service ");
    write_word(badge);
    for (uint32_t i = 0; *g_world.count && i < g_world.capacity; ++i) {
        if (g_world.table[i].badge != badge) {
            continue;
        }
        write(" (");
        write_name(g_world.table[i].name, g_world.table[i].name_length);
        write(")");
        break;
    }
    if (seL4_isVMFault_tag(info)) {
        /* Which address, and which instruction, are in the message for whoever
         * wants them: a supervisor's first job is to say who died, and the
         * detail belongs with the policy that decides what to do about it. The
         * two are printed because "who died" is not enough to find it. */
        write(" faulted on a memory access at ");
        aegir::debug_write_hex(addr);
        write(", pc ");
        aegir::debug_write_hex(pc);
        if (overflow != nullptr) {
            /* What ran out, with the numbers (AGENTS.md): the stack overflow's
             * own stack pointer and the range it exhausted. */
            write(" stack-overflow sp=");
            aegir::debug_write_hex(overflow->sp);
            write(" base=");
            aegir::debug_write_hex(overflow->base);
            write(" max=");
            aegir::debug_write_hex(overflow->max);
        } else if (why != nullptr && why[0] != '\0') {
            write(" stack-growth-failed ");
            write(why);
        }
    } else if (seL4_isUnknownSyscall_tag(info)) {
        write(" faulted on an unknown syscall");
    } else if (seL4_isNullFault_tag(info)) {
        write(" stopped without a fault");
    } else {
        write(" faulted (kind ");
        write_word(seL4_MessageInfo_get_label(info));
        write(")");
    }
    write("\n");
}

}  // namespace

}  // namespace aegir::director

/* The thread's entry point. It shares director's CSpace and VSpace, so this
 * function is directly where the thread starts. */
extern "C" [[noreturn]] void aegir_supervisor_entry()
{
    using aegir::director::Supervised;

    /* Said before anything else, and before any global is touched: a string
     * literal needs no global pointer, so this line answers "did the thread run"
     * without also depending on "did its data addresses work". */
    aegir::director::write("  supervisor: listening for faults\n");

    for (;;) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t const info = seL4_Recv(aegir::director::g_world.endpoint, &badge);
        /* The fault's own numbers, read before the pager's mem.main call can
         * replace the message registers (specs/pager.md). */
        uint64_t const fault_pc = seL4_isVMFault_tag(info) ? seL4_GetMR(0) : 0;
        uint64_t const fault_addr = seL4_isVMFault_tag(info) ? seL4_GetMR(1) : 0;

        /* The pager (specs/pager.md): a fault in a watched service's reserved
         * stack is a page to map, not an end. */
        bool handled = false;
        char const *why = "";
        aegir::spawn::StackFault stack_fault{};
        if (seL4_isVMFault_tag(info) && aegir::director::g_world.self != nullptr) {
            handled = aegir::director::g_world.self->grow(badge, fault_addr, &stack_fault, &why);
        }
        if (handled && !stack_fault.overflow && why[0] == '\0') {
            /* The page is mapped: let the faulting thread run again. */
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));
            continue;
        }

        ++(*aegir::director::g_world.faults);
        aegir::director::report_fault(
            badge, info, fault_pc, fault_addr,
            (handled && stack_fault.overflow) ? &stack_fault : nullptr, why);

        for (uint32_t i = 0; i < *aegir::director::g_world.count && i < aegir::director::g_world.capacity;
             ++i) {
            Supervised const &entry = aegir::director::g_world.table[i];
            if (entry.badge != badge) {
                continue;
            }
            /* Wake whoever is waiting to hear from this service. A service that
             * dies before it reports ready would otherwise leave the boot thread
             * waiting forever -- and the badge is what says which of the two
             * happened: the child signals with its own, this signal carries
             * director's. */
            if (entry.supervision != 0) {
                seL4_Signal(entry.supervision);
            }
            /* Stop it. It cannot be left running: it left its own control flow,
             * and a service that does not know what it is doing is not a service.
             * The manifest's restart rules come next (specs/director.md). */
            if (entry.tcb != 0) {
                seL4_TCB_Suspend(entry.tcb);
            }
            break;
        }
    }
}

namespace aegir::director {

Supervisor::Supervisor(mem::Allocator &allocator, mem::Scratch &scratch, mem::Arena &arena) noexcept
    : allocator_(allocator), scratch_(scratch), arena_(arena), table_(nullptr), capacity_(0),
      count_(nullptr), faults_(nullptr), problem_("no problem")
{
}

unsigned Supervisor::faults() const noexcept
{
    return faults_ != nullptr ? *faults_ : 0;
}

void Supervisor::record(uint32_t index, uint64_t badge, seL4_CPtr tcb, seL4_CPtr supervision,
                        char const *name, uint32_t name_length, PagerRecord const &pager) noexcept
{
    if (index >= capacity_) {
        return;
    }
    Supervised entry{};
    entry.badge = badge;
    entry.tcb = tcb;
    entry.supervision = supervision;
    entry.name = name;
    entry.name_length = name_length;
    entry.pager = pager;
    table_[index] = entry;
    *count_ = index + 1;
}

bool Supervisor::start(seL4_CPtr fault_endpoint, Supervised *table, uint32_t capacity,
                       mem::Account &account) noexcept
{
    table_ = table;
    capacity_ = capacity;
    count_ = static_cast<uint32_t *>(arena_.allocate(sizeof(uint32_t)));
    faults_ = static_cast<unsigned *>(arena_.allocate(sizeof(unsigned)));
    if (count_ == nullptr || faults_ == nullptr) {
        problem_ = "no memory for the supervisor's records";
        return false;
    }
    *count_ = 0;
    *faults_ = 0;

    /* The pager's slot pool (specs/pager.md): half of what director has free
     * now, kept for this thread alone, so the boot thread's allocator and the
     * pager never race. The frames and page tables are retyped from a child-
     * badged mem.main chunk into these plain root slots; when a service is
     * reaped, release(child) takes the memory back and the pool's slots are
     * returned. */
    seL4_Word const free_slots = allocator_.slots_total() - allocator_.slots_used();
    seL4_Word const pool_slots = free_slots / 2;
    if (pool_slots != 0) {
        seL4_CPtr const pool = allocator_.reserve_slot_run(pool_slots);
        if (pool != 0) {
            g_pager_mem.adopt_nodes(g_pager_nodes, sizeof(g_pager_nodes));
            g_pager_mem.adopt_slots(pool, pool_slots, seL4_WordBits, 0);
        }
    }

    seL4_Error error = seL4_NoError;
    seL4_CPtr const ipc_frame =
        allocator_.alloc_object(seL4_RISCV_4K_Page, seL4_PageBits, account, &error);
    if (ipc_frame == 0) {
        problem_ = "no memory for the supervisor's IPC buffer";
        return false;
    }
    void *const ipc_buffer = scratch_.map(ipc_frame);
    if (ipc_buffer == nullptr) {
        problem_ = "the supervisor's IPC buffer could not be mapped";
        return false;
    }

    /* Two pages of stack, mapped the same way and left mapped. The thread's TLS
     * block lives at the top of them and the stack grows down below it, which is
     * how upstream threads are built (projects/seL4_libs/libsel4utils/src/thread.c:169-177)
     * -- and it has to be per thread, because the IPC buffer pointer lives in TLS
     * and two threads must not share one. */
    uintptr_t stack_top = 0;
    for (unsigned page = 0; page < 2; ++page) {
        seL4_CPtr const frame =
            allocator_.alloc_object(seL4_RISCV_4K_Page, seL4_PageBits, account, &error);
        if (frame == 0) {
            problem_ = "no memory for the supervisor's stack";
            return false;
        }
        void *const at = scratch_.map(frame);
        if (at == nullptr) {
            problem_ = "the supervisor's stack could not be mapped";
            return false;
        }
        stack_top = reinterpret_cast<uintptr_t>(at) + kPage;
    }

    seL4_CPtr const tcb = allocator_.alloc_object(seL4_TCBObject, seL4_TCBBits, account, &error);
    if (tcb == 0) {
        problem_ = "no memory for the supervisor's TCB";
        return false;
    }
    /* Configured in *our* CSpace and VSpace, with the shared fault endpoint as its
     * own: a fault in the supervisor is then visible like any other rather than
     * silent. */
    error = seL4_TCB_Configure(tcb, fault_endpoint, seL4_CapInitThreadCNode, 0,
                               seL4_CapInitThreadVSpace, 0,
                               reinterpret_cast<seL4_Word>(ipc_buffer), ipc_frame);
    if (error != seL4_NoError) {
        problem_ = "the supervisor's TCB could not be configured";
        return false;
    }
    error = seL4_TCB_SetPriority(tcb, seL4_CapInitThreadTCB, seL4_MaxPrio - 1);
    if (error != seL4_NoError) {
        problem_ = "the supervisor's priority could not be set";
        return false;
    }

    g_world = World{fault_endpoint, table_, capacity_, count_, faults_, this};

    /* The thread's own TLS: the process's image, copied where this thread can
     * reach it, with its own IPC buffer pointer written into it. */
    uintptr_t const tls_size = sel4runtime_get_tls_size();
    if (tls_size == 0 || tls_size >= 2 * kPage) {
        problem_ = "the supervisor's TLS does not fit its stack";
        return false;
    }
    auto *tls_memory = reinterpret_cast<void *>(stack_top - tls_size);
    uintptr_t const thread_pointer = sel4runtime_write_tls_image(tls_memory);
    if (thread_pointer == 0) {
        problem_ = "the supervisor's TLS could not be written";
        return false;
    }
    seL4_IPCBuffer *ipc_pointer = static_cast<seL4_IPCBuffer *>(ipc_buffer);
    __sel4runtime_write_tls_variable(thread_pointer,
                                     reinterpret_cast<unsigned char *>(&__sel4_ipc_buffer),
                                     reinterpret_cast<unsigned char *>(&ipc_pointer),
                                     sizeof(ipc_pointer));
    error = seL4_TCB_SetTLSBase(tcb, thread_pointer);
    if (error != seL4_NoError) {
        problem_ = "the supervisor's TLS base could not be set";
        return false;
    }

    seL4_UserContext context = {};
    /* The thread has to be given the global pointer the rest of this process
     * uses. A process gets it from its crt -- the entry code computes it from
     * __global_pointer$ before anything else runs
     * (projects/sel4runtime/crt/arch/riscv/crt0.S:30-33) -- and a thread started
     * the way this one is skips all of that, so the first access to a global (the
     * world this thread works in) would fault. The value is whatever the running
     * thread has, because it is the same process. */
    seL4_Word gp = 0;
    asm volatile("mv %0, gp" : "=r"(gp));
    context.gp = gp;
    /* And the thread pointer, in the same context rather than only through
     * seL4_TCB_SetTLSBase below: WriteRegisters writes the whole user context, so
     * a zero here would overwrite the base that call had just set, leaving the
     * thread with no TLS -- which is a fault at address zero on its first access
     * to the IPC buffer (kernel/libsel4/include/sel4/functions.h:13). */
    context.tp = thread_pointer;
    context.pc = reinterpret_cast<seL4_Word>(&aegir_supervisor_entry);
    /* The stack pointer starts below the TLS block, not at the top of the stack:
     * that memory belongs to the thread's TLS now. */
    context.sp = (stack_top - tls_size) & ~static_cast<seL4_Word>(15);
    error = seL4_TCB_WriteRegisters(tcb, 1 /* resume */, 0, sizeof(context) / sizeof(seL4_Word),
                                     &context);
    if (error != seL4_NoError) {
        problem_ = "the supervisor could not be started";
        return false;
    }
    return true;
}

Supervised *Supervisor::find(uint64_t badge) noexcept
{
    if (table_ == nullptr || count_ == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < *count_ && i < capacity_; ++i) {
        if (table_[i].badge == badge) {
            return &table_[i];
        }
    }
    return nullptr;
}

seL4_CPtr Supervisor::pager_carve(uint64_t badge, seL4_Word type, seL4_Word size_bits,
                                  seL4_Error *error) noexcept
{
    Supervised *const entry = find(badge);
    if (entry == nullptr || !entry->pager.watched || mem_main_ == 0) {
        if (error != nullptr) {
            *error = seL4_InvalidCapability;
        }
        return 0;
    }
    if (entry->chunk == 0 || entry->chunk_left < kPage) {
        seL4_CPtr const call_slot = g_pager_mem.alloc_slot();
        seL4_CPtr const chunk_slot = g_pager_mem.alloc_slot();
        if (call_slot == 0 || chunk_slot == 0) {
            if (call_slot != 0) {
                g_pager_mem.free_slot(call_slot);
            }
            if (chunk_slot != 0) {
                g_pager_mem.free_slot(chunk_slot);
            }
            if (error != nullptr) {
                *error = seL4_NotEnoughMemory;
            }
            return 0;
        }
        /* The service records ownership by the *call's* badge, so the copy that
         * asks is minted with the child's id and the chunk is the child's
         * (specs/memory.md). */
        seL4_Word const depth = seL4_WordBits;
        if (seL4_CNode_Mint(seL4_CapInitThreadCNode, call_slot, depth,
                            seL4_CapInitThreadCNode, mem_main_, depth,
                            seL4_CapRights_new(1, 1, 0, 1), badge) != seL4_NoError) {
            g_pager_mem.free_slot(call_slot);
            g_pager_mem.free_slot(chunk_slot);
            if (error != nullptr) {
                *error = seL4_InvalidCapability;
            }
            return 0;
        }
        aegir::ipc::Consumer const service(call_slot);
        uint64_t const request = aegir::memory::kChunkBits;
        uint64_t answer[2] = {};
        bool cap_arrived = false;
        aegir::ipc::WordsReply const reply = service.call_transfer(
            aegir::memory::kMethodAlloc, &request, 1, 0, answer, 2, &cap_arrived);
        seL4_CNode_Delete(seL4_CapInitThreadCNode, call_slot, depth);
        g_pager_mem.free_slot(call_slot);
        if (reply.error != 0 || !cap_arrived ||
            !aegir::ipc::take_received_cap(chunk_slot)) {
            g_pager_mem.free_slot(chunk_slot);
            if (error != nullptr) {
                *error = seL4_NotEnoughMemory;
            }
            return 0;
        }
        entry->chunk = chunk_slot;
        entry->chunk_left = 1ull << (reply.count >= 1 ? answer[0] : request);
    }
    seL4_CPtr const slot =
        g_pager_mem.carve_object_from(entry->chunk, type, size_bits, g_pager_account, error);
    if (slot != 0) {
        entry->chunk_left -= kPage;
    }
    return slot;
}

void Supervisor::pager_drop(uint64_t badge, seL4_CPtr slot) noexcept
{
    static_cast<void>(badge);
    g_pager_mem.free_slot(slot);
}

seL4_CPtr Supervisor::pager_carve_cb(void *context, seL4_Word type, seL4_Word size_bits,
                                     seL4_Error *error) noexcept
{
    auto *const pager = static_cast<PagerContext *>(context);
    return pager->self->pager_carve(pager->badge, type, size_bits, error);
}

void Supervisor::pager_drop_cb(void *context, seL4_CPtr slot) noexcept
{
    auto *const pager = static_cast<PagerContext *>(context);
    pager->self->pager_drop(pager->badge, slot);
}

bool Supervisor::grow(uint64_t badge, uintptr_t address, StackFault *fault,
                      char const **why) noexcept
{
    Supervised *const entry = find(badge);
    if (entry == nullptr || !entry->pager.watched) {
        return false;
    }
    PagerContext context{this, badge};
    spawn::PagerSource const source{&Supervisor::pager_carve_cb,
                                    &Supervisor::pager_drop_cb, &context};
    bool const handled = spawn::page_stack(entry->pager.root, entry->pager.base,
                                           &entry->pager.lo, entry->pager.top,
                                           entry->pager.max, address, source, fault, why);
    if (handled && fault != nullptr && fault->overflow && entry->tcb != 0) {
        seL4_UserContext registers = {};
        if (seL4_TCB_ReadRegisters(entry->tcb, 0, 0,
                                   sizeof(registers) / sizeof(seL4_Word),
                                   &registers) == seL4_NoError) {
            fault->sp = registers.sp;
        }
    }
    return handled;
}

}  // namespace aegir::director
