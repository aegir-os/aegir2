/*
 * Creating a process: Aegir's own spawn path.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * This is the work specs/director.md lays out, with the mechanism of each step
 * taken from the pinned tree rather than invented:
 *
 *   1. the binary comes out of the flat initrd, by name;
 *   2. objects are retyped out of the account's untyped memory, so the cost is
 *      charged as it is spent (specs/authority.md);
 *   3. the child's CSpace is built with the Aegir slot layout, and its VSpace
 *      from the ELF's loadable segments (aegir/mem/child_vspace.h);
 *   4. it is given a stack carrying the frame a spawned program expects --
 *      argc/argv/envp/auxv -- where the auxv is what the runtime reads
 *      (projects/sel4runtime/src/env.c:282-320) plus Aegir's own entry pointing
 *      at the bootstrap block;
 *   5. its TCB is configured with the fault endpoint its spawner will hear from,
 *      and started at the ELF's entry point.
 *
 * It is deliberately not libsel4utils: specs/userland.md fixes that position, and
 * what we get for the work is a CSpace layout, a startup ABI and an accounting
 * rule that are Aegir's.
 */

#ifndef AEGIR_SPAWN_PROCESS_H
#define AEGIR_SPAWN_PROCESS_H

#include <aegir/bootstrap.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/child_vspace.h>
#include <aegir/mem/vspace.h>
#include <aegir/spawn/elf.h>
#include <aegir/spawn/initrd.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::spawn {

/** A port to install into the child before it starts (specs/services.md): the
 *  endpoint capability, the slot it goes in, and the rights for *this* side. Who
 *  gets which side is the graph's decision, not the spawner's -- the spawner's job
 *  is to install exactly what it is handed and say so in the block. */
struct PortGrant {
    char const *name;
    uint32_t name_length;
    uint64_t slot;
    seL4_CPtr capability;
    seL4_CapRights_t rights;
    /** The badge this copy carries: the child's own id when it is a caller, so the
     *  port's owner learns who called; zero for the copy it reads, because a
     *  receiver's badge is never what identifies it (specs/services.md). */
    uint64_t badge;
    /* For a capability that *is* memory -- an untyped a service is given to retype
     *  objects out of -- how big it is, in bits. Zero for everything else, which is
     *  every port: a port has no size, and a service cannot ask the kernel for one
     *  (there is no invocation that reads an untyped's size), so it has to be told.
     *  Last member on purpose: the places that build ports by aggregate
     *  initialization are then unchanged, and mean zero. */
    uint32_t size_bits = 0;
    /* Move the capability rather than minting a copy of it. There is exactly one
     *  reason: IRQControl derives to a *null* cap (kernel/src/object/objecttype.c:
     *  75-78), so the kernel's one well of handler caps cannot be copied -- custody
     *  of it changes hands whole, and the giver's slot is empty afterwards. Rights
     *  and badge do not apply to a move. Same placement rule as size_bits. */
    bool move = false;
    /* Copy the capability as it stands -- badge and rights preserved -- rather
     *  than minting. A parent that shares a *badged* port with a child needs
     *  this: a badged endpoint cap cannot be minted again (updateCapData
     *  refuses a non-zero badge, kernel/src/object/objecttype.c:402-407), so a
     *  session's command is handed the namespace the same way it would be
     *  copied -- carrying the session's identity. Rights and badge do not
     *  apply. Same placement rule as size_bits. (specs/dos.md) */
    bool copy = false;
};

/** A device frame handed over as a *capability* rather than a mapping: the child
 *  is told the slot and nothing is mapped. It maps the frame through a window of
 *  its own when it wants to read the registers, or hands it to a driver it starts
 *  -- which is what a device manager is for (specs/services.md). */
struct DeviceGrant {
    uint64_t physical;
    uint32_t bytes;
    seL4_CPtr frame;
};

/** What the manifest says about the process to create (specs/services.md). The
 *  strings are views into the manifest text, not copies, and the ports are the
 *  ones this process was granted -- ports it owns and ports it may call. */
struct Request {
    /* A flat blob to map into the child's address space and tell it about through
     *  the bootstrap block. The device tree travels this way: a device manager is
     *  given the machine's own description of itself rather than being told about
     *  hardware by its spawner. Null for a child that is given no such thing. */
    void const *devices = nullptr;
    uint32_t devices_bytes = 0;
    /* A device's register window to map into the child, for a service that drives
     *  one. Zero when the child is given no device. The capability stays the
     *  caller's: a mapping is what the child needs, not the frame. */
    seL4_CPtr device_frame = 0;
    uint32_t device_bytes = 0;
    /* Where that device's registers are in the machine's memory. Identical
     *  transports are told apart only by this, so a service that drives one needs
     *  it to know which one it was given. */
    uint64_t device_physical = 0;
    /* Memory the child is given to lay objects out in, when it needs some -- a driver's
     * virtqueue is the reason: a queue's descriptor entries carry *physical* addresses, and
     * only the spawner knows a region's physical base. Zero for a child that is given none.
     * The capability travels the same way a port's does (see `PortGrant::size_bits`), under
     * the name `untyped` (specs/services.md, specs/authority.md). */
    uint64_t untyped_physical = 0;
    uint32_t untyped_bits = 0;
    /* The page of memory director will map into this child, and where it will land. The
     * same shape as `device_frame`/`device_address`, and for a reason that only shows up
     * when it bites: a service cannot map into its own address space, because the spawner
     * holds the child's root page table (specs/services.md). So `memory_bytes` is what will
     * be mapped at `memory_address`, and the child is told the physical base separately. */
    seL4_CPtr memory_frame = 0;
    uint32_t memory_bytes = 0;
    /* The window this child's service port serves through, when it has one:
     * frames the caller carved, mapped into the child right after its memory,
     * with the physical base recorded so a driver can point virtqueue
     * descriptors at it. Each client is given its own frames (the spawner maps
     * them; a service cannot map into its own address space), because a window
     * shared by two clients is not safe under preemption (aegir/block.h,
     * specs/services.md). */
    seL4_CPtr window_frame = 0;
    uint32_t window_bytes = 0;
    uint64_t window_physical = 0;
    /* How wide one window frame is: seL4_PageBits (4 KiB) or
     * seL4_LargePageBits (a 2 MiB mega page, seL4_RISCV_Mega_Page). A window
     * big enough to be a framebuffer rides as a handful of mega pages, because
     * every frame in a set is a slot somebody's CSpace pays for
     * (specs/services.md). `window_frame + i` is frame `i' at
     * `window_physical + (i << window_page_bits)`, and the window is placed at
     * an address aligned to its frames -- a mega page's mapping needs a fresh
     * 2 MiB slot, not a page table already carrying 4 KiB leaves. */
    uint32_t window_page_bits = seL4_PageBits;
    /* A copy of the flat initrd, mapped read-only, for a process that starts
     * processes of its own: the binaries are the one part of spawning that cannot
     * be delegated as a capability, so they travel as bytes (specs/services.md).
     * Null for a child that may not spawn. */
    void const *binaries = nullptr;
    uint32_t binaries_bytes = 0;
    /* True when the process is trusted with its own VSpace root: the capability
     * arrives as a port named "vspace", and the block's Window entry says which of
     * the child's own addresses are free for it to map into. RISC-V has no
     * narrower mapping authority than the root, so this is the grant a spawner
     * needs (specs/services.md). */
    bool give_vspace = false;
    /* Device frames handed over as capabilities rather than mappings, for a child
     * that hands them on -- a device manager. The slots are assigned by the
     * spawner, right after the ports, and the block says where they are. */
    DeviceGrant const *device_grants = nullptr;
    uint32_t device_grant_count = 0;
    char const *name;
    uint32_t name_length;
    char const *binary;
    uint32_t binary_length;
    /* The binary's bytes directly, when the caller has them rather than an
     * initrd to look the name up in: the whole initrd is 1.2 MiB and a copy
     * per spawning service does not fit a service-sized delegation, so a
     * service that starts one known helper hands over just that helper's
     * image -- as bytes, because bytes are the one part of spawning that
     * cannot be delegated as a capability (specs/services.md). Null means the
     * name is looked up in the spawner's initrd, as before. */
    void const *binary_image = nullptr;
    uint64_t binary_image_bytes = 0;
    char const *account;
    uint32_t account_length;
    /* The arguments and environment the child starts with (specs/environment.md),
     * as the startup frame's argv/envp: NUL-terminated strings the spawner copies
     * into the child. argv[0] is always the program's name; `arguments` are what
     * follow it. `environment` is `NAME=VALUE` strings. A spawner normally passes
     * its own through -- inheritance -- which is what a shell does. */
    char const *const *arguments = nullptr;
    uint32_t argument_count = 0;
    char const *const *environment = nullptr;
    uint32_t environment_count = 0;
    /* The child's current directory, a VFS path (`Volume:component/path`), or
     * null/0 for none (specs/environment.md). */
    char const *cwd = nullptr;
    uint32_t cwd_length = 0;
    uint32_t priority;
    /** How many 4 KiB pages of stack the child is given. Zero takes the floor
     *  (kDefaultStackPages, 8 KiB); a process that runs the C++ standard
     *  library asks for more, because its container code is stack-hungry and an
     *  8 KiB stack overflows silently into whatever the spawner mapped below it
     *  (specs/userland.md's "a service's stack is what the spawner gives it"). */
    uint32_t stack_pages = 0;
    PortGrant const *ports;
    uint32_t port_count;
    /** The fault endpoint every service shares, and this service's badge on it.
     *  One endpoint plus a badge per child is what lets one supervisor watch them
     *  all and still know who it is looking at
     *  (kernel/src/kernel/faulthandler.c:41, :92). */
    seL4_CPtr fault_endpoint;
    uint64_t badge;
};

/** A created process, from its creator's side: the capabilities we hold for it. */
struct Process {
    seL4_CPtr cspace;         /* the child's root CNode */
    seL4_CPtr tcb;
    seL4_CPtr fault_endpoint; /* where its faults arrive, in our CSpace */
    seL4_CPtr supervision;    /* the notification it signals when it is ready */
    seL4_CPtr vspace_root;    /* its address space's root -- what a minted copy of the
                                 "vspace" grant names */
    uint64_t entry;
    uint64_t stack_top;
    uint64_t block;           /* the child's bootstrap block */
    uint64_t mapped_end;      /* the first page past everything spawn() mapped: where
                                 a child trusted with its own VSpace root begins its
                                 own window */
};

class Spawner {
public:
    /** A spawner over the authority it was given: the allocator holding its
     *  memory, the window it fills frames through, the ASID pool its children's
     *  address spaces come from -- director's own initial pool, or the one a
     *  service was delegated for exactly this (specs/authority.md) -- and the
     *  depth at which the spawner's own CSpace resolves plain slot numbers as a
     *  mint source. That is seL4_WordBits for the root task, whose initial
     *  CNode cap carries a guard over the high bits, and bootstrap::kCNodeBits
     *  for a service, whose own-CNode cap is a raw copy with guard 0 and radix
     *  kCNodeBits (kernel/src/kernel/cspace.c:126-193). The kernel offers no
     *  invocation to ask which; the spawner knows which it is. */
    Spawner(mem::Allocator &allocator, mem::Scratch &scratch, mem::Arena &arena,
            Initrd const &initrd, seL4_CPtr asid_pool, seL4_CPtr source_root,
            seL4_Word source_depth) noexcept;

    /** Create, load and start the process a manifest entry describes. */
    bool spawn(Request const &request, mem::Account &account, Process &process) noexcept;

    /** Why the last spawn failed: for the boot report, which is read by people. */
    char const *problem() const noexcept { return problem_; }

    /** Which step of the failing operation failed, when it reported one --
     *  "the bootstrap block could not be mapped" has three very different causes
     *  and the report is only useful if it says which. */
    char const *detail() const noexcept { return detail_; }

    /** The kernel's own answer to the last failed capability operation, when one
     *  was involved: seL4_NoError when the failure was not the kernel's. */
    uint64_t error() const noexcept { return static_cast<uint64_t>(error_); }

private:
    bool fail(char const *what) noexcept;
    bool install(seL4_CPtr into_cspace, uint64_t slot, seL4_CPtr source,
                 seL4_CapRights_t rights, uint64_t badge) noexcept;
    /** Move rather than mint: for the caps a copy cannot carry (PortGrant.move). */
    bool install_moved(seL4_CPtr into_cspace, uint64_t slot, seL4_CPtr source) noexcept;
    /** Copy rather than mint, preserving the source's badge (PortGrant.copy). */
    bool install_copied(seL4_CPtr into_cspace, uint64_t slot, seL4_CPtr source) noexcept;
    /** Lay out argc/argv/envp/auxv on the child's stack. Returns the stack
     *  pointer, or 0 when it does not fit. */
    uintptr_t build_start_frame(uint8_t *stack, uint64_t stack_size, uintptr_t stack_top,
                                Elf const &elf, Request const &request, uintptr_t block,
                                uintptr_t ipc_buffer) noexcept;

    mem::Allocator &allocator_;
    mem::Scratch &scratch_;
    mem::Arena &arena_;
    Initrd const &initrd_;
    seL4_CPtr asid_pool_;
    seL4_CPtr source_root_;
    seL4_Word source_depth_;
    /* The one read-only copy of a `binaries` blob, shared by every child that
     * is given it: the frames are made on the first spawn that asks and
     * *mapped* -- not copied -- into each later one, because a copy per
     * spawner is the cost that filled the allocator's untyped table when the
     * second spawner arrived (specs/services.md). The blob's address is the
     * identity: there is one initrd, and a spawn handed a *different* blob
     * than the one the frames hold fails loudly rather than reading the
     * wrong archive. */
    void const *shared_binaries_ = nullptr;
    seL4_CPtr *shared_binaries_frames_ = nullptr;
    uint32_t shared_binaries_pages_ = 0;
    char const *problem_;
    char const *detail_;
    seL4_Error error_;
};

}  // namespace aegir::spawn

#endif  // AEGIR_SPAWN_PROCESS_H
