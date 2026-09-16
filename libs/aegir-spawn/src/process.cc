/*
 * Creating a process -- implementation. See include/aegir/spawn/process.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/spawn/process.h>

#include <sel4runtime/auxv.h>

namespace aegir::spawn {

namespace {

constexpr uint64_t kPage = 1ull << seL4_PageBits;

/* What a service starts with. Both of these are capacities, so both belong in the
 * manifest rather than in a header (specs/services.md): until a service declares
 * them, these are the sizes for one whose only job is to exist and say so. */
constexpr uint32_t kCNodeBits = 10;   /* 2^10 slots of CSpace */
constexpr unsigned kStackPages = 2;   /* 8 KiB of stack */
constexpr uint64_t kBlockBytes = 512; /* the bootstrap block, in its own page */

constexpr uint64_t kAuxvEntrySize = 16; /* int plus a word, padded */
constexpr uint32_t kAuxvEntries = 8;    /* seven below, plus AT_NULL */

uintptr_t align_up(uintptr_t value, uintptr_t alignment) noexcept
{
    return (value + alignment - 1) & ~(alignment - 1);
}

uintptr_t align_down(uintptr_t value, uintptr_t alignment) noexcept
{
    return value & ~(alignment - 1);
}

void write_word(uint8_t *stack, uintptr_t stack_lo, uintptr_t address, uint64_t value) noexcept
{
    uint8_t *at = stack + (address - stack_lo);
    for (unsigned i = 0; i < 8; ++i) {
        at[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

void write_auxv(uint8_t *stack, uintptr_t stack_lo, uintptr_t address, int type,
                uint64_t value) noexcept
{
    /* auxv_t is an int, its padding, and a word. */
    write_word(stack, stack_lo, address, static_cast<uint32_t>(type));
    write_word(stack, stack_lo, address + 8, value);
}

}  // namespace

Spawner::Spawner(mem::Allocator &allocator, mem::Scratch &scratch, mem::Arena &arena,
                 Initrd const &initrd) noexcept
    : allocator_(allocator), scratch_(scratch), arena_(arena), initrd_(initrd),
      problem_("no problem")
{
}

bool Spawner::fail(char const *what) noexcept
{
    problem_ = what;
    return false;
}

bool Spawner::install(seL4_CPtr into_cspace, uint64_t slot, seL4_CPtr source,
                      seL4_CapRights_t rights, uint64_t badge) noexcept
{
    /* The destination is a single-level CNode with no guard, so a slot is
     * addressed by index at the node's own depth; the source is a slot in our
     * root CNode, which is addressed at the full word depth
     * (projects/seL4_libs/libsel4allocman/src/bootstrap.c:434-440).
     *
     * Minting rather than copying, even when the badge is zero: the badge is how
     * a child is identified to whoever it talks to, and it has to come from the
     * cap it uses rather than from anything it says about itself. */
    return seL4_CNode_Mint(into_cspace, slot, kCNodeBits, seL4_CapInitThreadCNode, source,
                           seL4_WordBits, rights, badge) == seL4_NoError;
}

uintptr_t Spawner::build_start_frame(uint8_t *stack, uint64_t stack_size, uintptr_t stack_top,
                                     Elf const &elf, Request const &request, uintptr_t block,
                                     uintptr_t ipc_buffer) noexcept
{
    /* The frame a spawned program is started with: argc, argv, envp, auxv -- and
     * the runtime reads exactly this (projects/sel4runtime/src/env.c:282-320).
     * Sizes first, then placement upward from a 16-byte aligned stack pointer,
     * because the ABI wants sp aligned at entry and a child that starts
     * misaligned cannot tell anyone why. */
    uintptr_t const stack_lo = stack_top - stack_size;
    uint64_t const name_bytes = request.name_length + 1;
    uint64_t const phdr_bytes =
        static_cast<uint64_t>(elf.program_headers()) * elf.program_header_size();

    uint64_t total = 0;
    total += 8;                             /* argc */
    total += 8 * 2;                         /* argv[0] and its terminator */
    total += 8;                             /* envp terminator: no environment */
    total += kAuxvEntries * kAuxvEntrySize; /* the auxv */
    total += align_up(name_bytes, 8);       /* the program name, above it all */
    total += align_up(phdr_bytes, 8);       /* the program headers, higher still */
    if (total > stack_size) {
        return 0;
    }

    uintptr_t const sp = align_down(stack_top - total, 16);
    if (sp < stack_lo) {
        return 0;
    }
    uintptr_t cursor = sp;
    uintptr_t const argc_at = cursor;
    cursor += 8;
    uintptr_t const argv_at = cursor;
    cursor += 8 * 2;
    uintptr_t const envp_at = cursor;
    cursor += 8;
    uintptr_t const auxv_at = cursor;
    cursor += kAuxvEntries * kAuxvEntrySize;
    uintptr_t const name_at = cursor;
    cursor += align_up(name_bytes, 8);
    uintptr_t const phdr_at = cursor;

    write_word(stack, stack_lo, argc_at, 1);
    write_word(stack, stack_lo, argv_at, name_at);
    write_word(stack, stack_lo, argv_at + 8, 0);
    write_word(stack, stack_lo, envp_at, 0);

    struct Aux {
        int type;
        uint64_t value;
    };
    Aux const entries[] = {
        {AT_PAGESZ, 1ull << seL4_PageBits},
        {AT_PHDR, phdr_at},
        {AT_PHNUM, elf.program_headers()},
        {AT_PHENT, elf.program_header_size()},
        {AT_SEL4_IPC_BUFFER_PTR, ipc_buffer},
        /* Its own TCB slot: the runtime uses it for thread-local bookkeeping. */
        {AT_SEL4_TCB, bootstrap::kSlotOwnTcb},
        /* Aegir's own entry: the block that says who this process is. */
        {bootstrap::kAuxvTag, block},
    };
    for (uint32_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        write_auxv(stack, stack_lo, auxv_at + i * kAuxvEntrySize, entries[i].type,
                   entries[i].value);
    }
    write_auxv(stack, stack_lo, auxv_at + (kAuxvEntries - 1) * kAuxvEntrySize, AT_NULL, 0);

    uint8_t *name = stack + (name_at - stack_lo);
    for (uint32_t i = 0; i < request.name_length; ++i) {
        name[i] = request.name[i];
    }
    name[request.name_length] = '\0';
    if (!elf.copy_program_headers(stack + (phdr_at - stack_lo), phdr_bytes)) {
        return 0;
    }
    return sp;
}

bool Spawner::spawn(Request const &request, mem::Account &account, Process &process) noexcept
{
    problem_ = "no problem";

    uint64_t elf_size = 0;
    void const *image = initrd_.find(request.binary, request.binary_length, &elf_size);
    if (image == nullptr) {
        return fail("the manifest names a binary the initrd does not contain");
    }
    Elf elf;
    if (!elf.parse(image, elf_size)) {
        return fail("that binary is not a loadable RISC-V 64 ELF");
    }

    seL4_Error error = seL4_NoError;
    /* The size to ask for is the CNode's size in slots-bits: the kernel adds
     * SlotBits itself when it works out the memory the object needs
     * (kernel/src/object/objecttype.c:45-46) and creates the CNode with exactly
     * the number passed (":557-563"). Passing the sum asks for 32x the memory
     * and gets "Insufficient memory" for a CSpace that would have fit. */
    process.cspace = allocator_.alloc_object(seL4_CapTableObject, kCNodeBits, account, &error);
    if (process.cspace == 0) {
        return fail("no memory for the child's CSpace");
    }

    mem::ChildVSpace vspace(allocator_, scratch_);
    if (!vspace.create(account)) {
        return fail("no memory for the child's address space");
    }

    /* The program's own pages, at the addresses it was linked for. A segment
     * starts wherever the linker put it -- aegir-hello's data segment begins at
     * 0x13da0 -- so each one is mapped from the page base and its bytes land at
     * the offset within that page. */
    uintptr_t mapped_until = 0;
    for (uint32_t i = 0; i < elf.program_headers(); ++i) {
        ProgramHeader header = elf.program_header(i);
        if (header.type != Elf::kProgramHeaderLoad || header.memsz == 0) {
            continue;
        }
        uintptr_t const page_base = static_cast<uintptr_t>(header.vaddr) & ~(kPage - 1);
        uint64_t const leading = header.vaddr - page_base;
        uint64_t const span = leading + header.memsz;
        if (span / kPage >= 0x1000000ull) {
            return fail("a segment of the program is implausibly large");
        }
        uint64_t const pages = (span + kPage - 1) / kPage;
        if (page_base < mapped_until) {
            /* Two segments sharing a page would mean mapping it twice, the second
             * time over the first one's bytes. Refusing is honest; handling it
             * means copying both into one frame, which is work for the day a
             * program needs it. */
            return fail("two loadable segments share a page, which is not handled yet");
        }
        if (!vspace.populate(page_base, static_cast<unsigned>(pages), elf.segment_bytes(header),
                             header.filesz, leading, true, account)) {
            return fail("a segment of the program could not be mapped");
        }
        mapped_until = page_base + static_cast<uintptr_t>(pages) * kPage;
    }

    /* Everything else goes above the image, so a bigger program cannot collide
     * with it: the block, the IPC buffer, then the stack. */
    uintptr_t const block_at = align_up(static_cast<uintptr_t>(elf.load_end()), kPage);
    uintptr_t const ipc_at = block_at + kPage;
    uintptr_t const stack_lo = ipc_at + kPage;
    uintptr_t const stack_top = stack_lo + kStackPages * kPage;
    uint64_t const stack_bytes = kStackPages * kPage;

    auto *block_storage = static_cast<uint8_t *>(arena_.allocate(kPage));
    if (block_storage == nullptr) {
        return fail("no memory for the bootstrap block");
    }
    /* The block describes the ports as well as the process, because they are part
     * of who it is: the child finds a port by name and the slot stays a layout
     * detail it did not choose (specs/services.md). */
    auto *port_entries =
        static_cast<bootstrap::PortEntry *>(arena_.allocate(sizeof(bootstrap::PortEntry) *
                                                           (request.port_count + 1)));
    if (port_entries == nullptr) {
        return fail("no memory for the bootstrap block's port list");
    }
    for (uint32_t i = 0; i < request.port_count; ++i) {
        port_entries[i].name = request.ports[i].name;
        port_entries[i].name_length = request.ports[i].name_length;
        port_entries[i].slot = request.ports[i].slot;
    }
    /* A blob the caller wants the child to have -- the device tree, for the device
     * manager. It goes above the stack so a bigger program cannot collide with it,
     * and the child reads it in place. */
    uint64_t devices_address = 0;
    uint64_t devices_end = stack_top;
    if (request.devices != nullptr && request.devices_bytes > 0) {
        uint64_t const pages = (request.devices_bytes + kPage - 1) / kPage;
        if (!vspace.populate(stack_top, static_cast<unsigned>(pages), request.devices,
                             request.devices_bytes, 0, false, account)) {
            return fail("the blob the child was to be given could not be mapped");
        }
        devices_address = stack_top;
        devices_end = stack_top + pages * kPage;
    }

    /* A device's registers, if the caller gave one: mapped above everything else, so
     * it cannot collide with the image or the blob, and recorded in the block. The
     * frame is *mapped* rather than given -- the capability stays with whoever
     * retyped it out of the machine's device memory (specs/services.md). */
    uint64_t device_address = 0;
    if (request.device_frame != 0 && request.device_bytes > 0) {
        if (request.device_bytes > kPage) {
            /* One frame is one page of registers, and the transports are 4 KiB
             * each. A wider window would need one frame per page and the caller to
             * say which; refusing is honest. */
            return fail("a device window wider than one page is not handled yet");
        }
        uintptr_t const at = align_up(static_cast<uintptr_t>(devices_end), kPage);
        if (!vspace.map_page(at, request.device_frame, true, account)) {
            return fail("the device's registers could not be mapped into the child");
        }
        device_address = at;
    }
    if (bootstrap::write(block_storage, kBlockBytes, request.name, request.name_length,
                         request.account, request.account_length, port_entries,
                         request.port_count, devices_address, request.devices_bytes,
                         device_address, request.device_bytes) == nullptr) {
        return fail("the bootstrap block does not fit its page");
    }
    if (!vspace.populate(block_at, 1, block_storage, kBlockBytes, 0, false, account)) {
        return fail("the bootstrap block could not be mapped");
    }

    /* The IPC buffer's frame capability is part of the TCB configuration, so it
     * comes back out of the mapping. */
    seL4_CPtr ipc_frame = 0;
    if (!vspace.populate(ipc_at, 1, nullptr, 0, 0, true, account, &ipc_frame)) {
        return fail("no memory for the child's IPC buffer");
    }

    auto *stack = static_cast<uint8_t *>(arena_.allocate(stack_bytes));
    if (stack == nullptr) {
        return fail("no memory for the child's stack");
    }
    uintptr_t const sp =
        build_start_frame(stack, stack_bytes, stack_top, elf, request, block_at, ipc_at);
    if (sp == 0) {
        return fail("the startup frame does not fit on the child's stack");
    }
    if (!vspace.populate(stack_lo, kStackPages, stack, stack_bytes, 0, true, account)) {
        return fail("the child's stack could not be mapped");
    }

    process.tcb = allocator_.alloc_object(seL4_TCBObject, seL4_TCBBits, account, &error);
    if (process.tcb == 0) {
        return fail("no memory for the child's TCB");
    }
    process.fault_endpoint = request.fault_endpoint;
    if (process.fault_endpoint == 0) {
        return fail("the caller did not provide a fault endpoint to share");
    }
    process.supervision =
        allocator_.alloc_object(seL4_NotificationObject, seL4_NotificationBits, account, &error);
    if (process.supervision == 0) {
        return fail("no memory for the supervision notification");
    }

    /* Its own CSpace and TCB, so it can name itself; the supervisor's fault
     * endpoint, carrying its badge; and the notification it signals when it is
     * ready -- write rights only, because signalling is all it needs to do. */
    /* Slots 1 and 2 are seL4's: a child that does not have its TCB at slot 1 and
     * its CNode at slot 2 is stopped by the kernel as soon as it names itself
     * (aegir/bootstrap.h explains). */
    if (!install(process.cspace, bootstrap::kSlotOwnTcb, process.tcb, seL4_AllRights, 0)) {
        return fail("the child's own TCB could not be given to it");
    }
    if (!install(process.cspace, bootstrap::kSlotOwnCNode, process.cspace, seL4_AllRights, 0)) {
        return fail("the child's own CSpace could not be given to it");
    }
    /* The fault endpoint needs Write and Grant-or-GrantReply, which is the
     * kernel's own requirement of a capability it delivers faults to
     * (out/aegir/libsel4/include/interfaces/sel4_client.h:1202) -- the same rule
     * a caller's port capability satisfies. */
    if (!install(process.cspace, bootstrap::kSlotFaultEndpoint, process.fault_endpoint,
                 seL4_AllRights, request.badge)) {
        return fail("the child's fault endpoint could not be installed");
    }
    if (!install(process.cspace, bootstrap::kSlotSupervision, process.supervision, seL4_CanWrite,
                 request.badge)) {
        return fail("the supervision notification could not be installed");
    }

    /* The ports, each with the rights its side of the port calls for: an owner
     * gets Read (it receives; replying needs nothing from the endpoint), a caller
     * gets Write and GrantReply. The second half is not a guess: the kernel
     * requires "both Write rights and either Grant or GrantReply" of a capability
     * that may be called, and says so for fault endpoints in
     * out/aegir/libsel4/include/interfaces/sel4_client.h:1202. A call that the
     * kernel refuses comes back as an error label, which libs/aegir-ipc reports,
     * so a wrong guess here is visible rather than silent. */
    for (uint32_t i = 0; i < request.port_count; ++i) {
        PortGrant const &grant = request.ports[i];
        if (grant.capability == 0 || !install(process.cspace, grant.slot, grant.capability,
                                              grant.rights, grant.badge)) {
            return fail("a port could not be installed into the child");
        }
    }

    /* Configure, then start. The fault endpoint is named in the child's CSpace --
     * that is what "this capability is in the CSpace of the thread being
     * configured" means (out/aegir/libsel4/include/interfaces/sel4_client.h:876)
     * -- while the CSpace, VSpace and IPC buffer frame are ours. */
    /* The child's CSpace gets a guard covering the bits its CNode does not index.
     * A kernel-created root CNode comes with one -- that is why
     * projects/seL4_libs/libsel4allocman/src/bootstrap.c:434-440 sets
     * cnode_guard_bits = seL4_WordBits - cnode_size_bits -- and without it
     * nothing can address the child's slots the way every program and library
     * does: seL4_CapInitThreadTCB is the name `1`, and a full-depth lookup that
     * runs out of address before it runs out of depth fails. The runtime then
     * tries to name itself with a capability the kernel cannot resolve, and is
     * stopped with "cap is not a TCB" before main ever runs. */
    seL4_Word const cspace_guard =
        seL4_CNode_CapData_new(0, seL4_WordBits - kCNodeBits).words[0];
    error = seL4_TCB_Configure(process.tcb, bootstrap::kSlotFaultEndpoint, process.cspace,
                               cspace_guard, vspace.root(), 0, ipc_at, ipc_frame);
    if (error != seL4_NoError) {
        return fail("the child's TCB could not be configured");
    }
    error = seL4_TCB_SetPriority(process.tcb, seL4_CapInitThreadTCB, request.priority);
    if (error != seL4_NoError) {
        return fail("the child's priority could not be set");
    }

    seL4_UserContext context = {};
    context.pc = elf.entry();
    context.sp = sp;
    /* gp and a0 are the startup code's business, not ours: the crt sets the
     * global pointer and derives its argument from sp
     * (projects/sel4runtime/crt/arch/riscv/crt0.S:30-37). */
    error = seL4_TCB_WriteRegisters(process.tcb, 1 /* resume */, 0,
                                    sizeof(context) / sizeof(seL4_Word), &context);
    if (error != seL4_NoError) {
        return fail("the child could not be started");
    }

    process.entry = elf.entry();
    process.stack_top = stack_top;
    process.block = block_at;
    return true;
}

}  // namespace aegir::spawn
