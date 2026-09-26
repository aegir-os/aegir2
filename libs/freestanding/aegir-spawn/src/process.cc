/*
 * Creating a process -- implementation. See include/aegir/spawn/process.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/bootstrap.h>
#include <aegir/spawn/process.h>

#include <sel4runtime/auxv.h>

namespace aegir::spawn {

namespace {

constexpr uint64_t kPage = 1ull << seL4_PageBits;

/* What a service starts with. Both of these are capacities, so both belong in the
 * manifest rather than in a header (specs/services.md): until a service declares
 * them, these are the sizes for one whose only job is to exist and say so. The
 * CSpace size is layout, not a default: a service that itself spawns addresses
 * its own CNode through it, so it lives with the other block layout constants in
 * aegir/bootstrap.h (kCNodeBits). */
/* 64 KiB: a hosted C++ program runs libc++ and std::filesystem on its stack,
 * and 8 KiB -- the floor this used to be -- overflowed on a line with a stat
 * in it (the shell, specs/shell.md). A service that knows it needs more says
 * so with `stack_kib`; this is the floor, not a ceiling. */
constexpr unsigned kDefaultStackPages = 16;
/* The bootstrap block fills the page it is mapped as: what a process is given
 * is part of who it is, and a service with many grants -- the partition
 * manager carries a window's frame per page -- is not a smaller kind of
 * process. */
constexpr uint64_t kBlockBytes = kPage;

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

uint64_t c_strlen(char const *text) noexcept
{
    uint64_t n = 0;
    while (text[n] != '\0') {
        ++n;
    }
    return n;
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
                 Initrd const &initrd, seL4_CPtr asid_pool, seL4_CPtr source_root,
                 seL4_Word source_depth) noexcept
    : allocator_(allocator), scratch_(scratch), arena_(arena), initrd_(initrd),
      asid_pool_(asid_pool), source_root_(source_root), source_depth_(source_depth),
      problem_("no problem"), detail_(""), error_(seL4_NoError)
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
     * own root CNode, whose addressing is the caller's business -- the root
     * task's initial CNode cap carries a guard over the high bits, so plain
     * slots resolve at full word depth
     * (projects/seL4_libs/libsel4allocman/src/bootstrap.c:434-440), while a
     * service's own-CNode cap is a raw copy with guard 0 and radix kCNodeBits,
     * and resolves plain slots at that depth instead
     * (kernel/src/kernel/cspace.c:126-193). The kernel offers no invocation to
     * ask which of these the caller is, so the caller says.
     *
     * Minting rather than copying, even when the badge is zero: the badge is how
     * a child is identified to whoever it talks to, and it has to come from the
     * cap it uses rather than from anything it says about itself. */
    seL4_Error const mint_error =
        seL4_CNode_Mint(into_cspace, slot, bootstrap::kCNodeBits, source_root_, source,
                        source_depth_, rights, badge);
    if (mint_error != seL4_NoError) {
        detail_ = "installing a capability into the child's CSpace";
        error_ = mint_error;
        return false;
    }
    return true;
}

bool Spawner::install_moved(seL4_CPtr into_cspace, uint64_t slot, seL4_CPtr source) noexcept
{
    /* Move takes the capability as it stands -- no derive, so the caps a copy
     *  reduces to nothing (IRQControl: kernel/src/object/objecttype.c:75-78)
     *  cross whole, and the source slot is empty from then on. The addressing
     *  is install()'s: a plain slot in the destination's own-depth CNode, the
     *  caller's root and depth on the source side. */
    seL4_Error const move_error =
        seL4_CNode_Move(into_cspace, slot, bootstrap::kCNodeBits, source_root_, source,
                        source_depth_);
    if (move_error != seL4_NoError) {
        detail_ = "moving a capability into the child's CSpace";
        error_ = move_error;
        return false;
    }
    return true;
}

bool Spawner::install_copied(seL4_CPtr into_cspace, uint64_t slot, seL4_CPtr source) noexcept
{
    /* Copy, not mint: the capability crosses as it stands, its badge preserved.
     * A badged endpoint cap cannot be minted again -- updateCapData refuses a
     * non-zero badge (kernel/src/object/objecttype.c:402-407) -- so a parent
     * that shares a badged port with a child copies it, and the copy keeps the
     * identity the parent's own cap carried (specs/dos.md). The addressing is
     * install()'s. maskCapRights leaves an endpoint cap unchanged, so the
     * rights argument does not narrow what the source held. */
    seL4_Error const copy_error =
        seL4_CNode_Copy(into_cspace, slot, bootstrap::kCNodeBits, source_root_, source,
                        source_depth_, seL4_AllRights);
    if (copy_error != seL4_NoError) {
        detail_ = "copying a capability into the child's CSpace";
        error_ = copy_error;
        return false;
    }
    return true;
}

uintptr_t Spawner::build_start_frame(uint8_t *stack, uint64_t stack_size, uintptr_t stack_top,
                                     Elf const &elf, Request const &request, uintptr_t block,
                                     uintptr_t ipc_buffer) noexcept
{
    /* The frame a spawned program is started with: argc, argv, envp, auxv -- and
     * the runtime reads exactly this (projects/sel4runtime/src/env.c:282-320,
     * crt1.c). argv[0] is the program's name; the request's arguments follow it,
     * and the request's environment is envp (specs/environment.md). Sizes first,
     * then placement upward from a 16-byte aligned stack pointer, because the ABI
     * wants sp aligned at entry and a child that starts misaligned cannot tell
     * anyone why. */
    uintptr_t const stack_lo = stack_top - stack_size;
    uint32_t const argc = 1 + request.argument_count;
    uint32_t const envc = request.environment_count;
    uint64_t const name_bytes = request.name_length + 1;
    uint64_t const phdr_bytes =
        static_cast<uint64_t>(elf.program_headers()) * elf.program_header_size();

    uint64_t strings = align_up(name_bytes, 8);
    for (uint32_t i = 0; i < request.argument_count; ++i) {
        strings += align_up(c_strlen(request.arguments[i]) + 1, 8);
    }
    for (uint32_t i = 0; i < envc; ++i) {
        strings += align_up(c_strlen(request.environment[i]) + 1, 8);
    }

    uint64_t total = 0;
    total += 8;                             /* argc */
    total += 8 * (argc + 1);                /* argv and its terminator */
    total += 8 * (envc + 1);                /* envp and its terminator */
    total += kAuxvEntries * kAuxvEntrySize; /* the auxv */
    total += strings;                       /* the strings, above the vectors */
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
    cursor += 8 * (argc + 1);
    uintptr_t const envp_at = cursor;
    cursor += 8 * (envc + 1);
    uintptr_t const auxv_at = cursor;
    cursor += kAuxvEntries * kAuxvEntrySize;
    uintptr_t const strings_at = cursor;
    uintptr_t const phdr_at = cursor + strings;

    write_word(stack, stack_lo, argc_at, argc);

    /* The strings are copied in above the vectors; `at` walks them and the
     * pointer written into argv/envp is where each landed. */
    uintptr_t at = strings_at;
    auto put_string = [&](char const *text, uint64_t bytes) noexcept {
        uint8_t *out = stack + (at - stack_lo);
        for (uint64_t i = 0; i + 1 < bytes; ++i) {
            out[i] = text[i];
        }
        out[bytes - 1] = '\0';
        uintptr_t const here = at;
        at += align_up(bytes, 8);
        return here;
    };
    write_word(stack, stack_lo, argv_at, put_string(request.name, name_bytes));
    for (uint32_t i = 0; i < request.argument_count; ++i) {
        uint64_t const bytes = c_strlen(request.arguments[i]) + 1;
        write_word(stack, stack_lo, argv_at + 8 * (i + 1),
                   put_string(request.arguments[i], bytes));
    }
    write_word(stack, stack_lo, argv_at + 8 * argc, 0);
    for (uint32_t i = 0; i < envc; ++i) {
        uint64_t const bytes = c_strlen(request.environment[i]) + 1;
        write_word(stack, stack_lo, envp_at + 8 * i,
                   put_string(request.environment[i], bytes));
    }
    write_word(stack, stack_lo, envp_at + 8 * envc, 0);

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

    if (!elf.copy_program_headers(stack + (phdr_at - stack_lo), phdr_bytes)) {
        return 0;
    }
    return sp;
}

bool Spawner::spawn(Request const &request, mem::Account &account, Process &process) noexcept
{
    problem_ = "no problem";
    detail_ = "";
    error_ = seL4_NoError;
    char const *why = nullptr;

    uint64_t elf_size = 0;
    void const *image = nullptr;
    if (request.binary_image != nullptr && request.binary_image_bytes > 0) {
        image = request.binary_image;
        elf_size = request.binary_image_bytes;
    } else {
        image = initrd_.find(request.binary, request.binary_length, &elf_size);
    }
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
    process.cspace = allocator_.alloc_object(seL4_CapTableObject, bootstrap::kCNodeBits, account,
                                             &error);
    if (process.cspace == 0) {
        return fail("no memory for the child's CSpace");
    }

    mem::ChildVSpace vspace(allocator_, scratch_);
    if (!vspace.create(asid_pool_, account)) {
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
                             header.filesz, leading, true, account, nullptr, &why)) {
            detail_ = why;
            return fail("a segment of the program could not be mapped");
        }
        mapped_until = page_base + static_cast<uintptr_t>(pages) * kPage;
    }

    /* Everything else goes above the image, so a bigger program cannot collide
     * with it: the block, the IPC buffer, then the stack. */
    uintptr_t const block_at = align_up(static_cast<uintptr_t>(elf.load_end()), kPage);
    uintptr_t const ipc_at = block_at + kPage;
    uintptr_t const stack_lo = ipc_at + kPage;
    unsigned const stack_pages =
        request.stack_pages != 0 ? request.stack_pages : kDefaultStackPages;
    uintptr_t const stack_top = stack_lo + stack_pages * kPage;
    uint64_t const stack_bytes = stack_pages * kPage;

    auto *block_storage = static_cast<uint8_t *>(arena_.allocate(kPage));
    if (block_storage == nullptr) {
        return fail("no memory for the bootstrap block");
    }
    /* The block describes the ports as well as the process, because they are part
     * of who it is: the child finds a port by name and the slot stays a layout
     * detail it did not choose (specs/services.md). One spare entry, for the
     * "vspace" grant when the child is trusted with its own address space. */
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
        port_entries[i].size_bits = request.ports[i].size_bits;
    }
    /* Declared slots are handed out from kSlotFirstDeclared upward: the ports the
     * caller named, then the vspace grant, then the device capabilities. The
     * block and the installs below use the same arithmetic, which is what makes
     * the two agree. */
    uint32_t port_count = request.port_count;
    uint64_t const vspace_slot = bootstrap::kSlotFirstDeclared + port_count;
    if (request.give_vspace) {
        port_entries[port_count] =
            bootstrap::PortEntry{"vspace", 6, vspace_slot, 0};
        ++port_count;
    }
    uint64_t const device_slot_base = bootstrap::kSlotFirstDeclared + port_count;
    /* A blob the caller wants the child to have -- the device tree, for the device
     * manager. It goes above the stack so a bigger program cannot collide with it,
     * and the child reads it in place. */
    uint64_t devices_address = 0;
    uint64_t devices_end = stack_top;
    if (request.devices != nullptr && request.devices_bytes > 0) {
        uint64_t const pages = (request.devices_bytes + kPage - 1) / kPage;
        if (!vspace.populate(stack_top, static_cast<unsigned>(pages), request.devices,
                             request.devices_bytes, 0, false, account, nullptr, &why)) {
            detail_ = why;
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
        if ((request.device_bytes % kPage) != 0) {
            return fail("a device window that is not a whole number of pages");
        }
        /* One frame per page, mapped consecutively, so a service's window is as
         * contiguous as the machine's memory is and an offset into one is an offset
         * into the other. The frames are the caller's and they sit in consecutive
         * slots -- which is how the allocator hands slots out (libs/aegir-mem) -- so
         * page `i` of the window is the capability `device_frame + i`. */
        uint32_t const pages = request.device_bytes / static_cast<uint32_t>(kPage);
        uintptr_t const at = align_up(static_cast<uintptr_t>(devices_end), kPage);
        for (uint32_t i = 0; i < pages; ++i) {
            seL4_Error mapped = seL4_NoError;
            if (!vspace.map_page(at + i * kPage, request.device_frame + i, true, account, &mapped)) {
                /* The kernel's own answer, because "it did not work" has several and
                 * they mean different things (kernel/manual/parts/vspace.tex, and the
                 * branches in kernel/src/arch/riscv/kernel/vspace.c). */
                switch (mapped) {
                case seL4_InvalidCapability:
                    return fail("a device frame does not belong to the child's address space");
                case seL4_FailedLookup:
                    return fail("the page tables above a device's address could not be made");
                default:
                    return fail("a device frame could not be mapped into the child");
                }
            }
        }
        device_address = at;
    }
    /* The initrd copy, for a child that starts processes of its own: mapped above
     * the device window, read in place, and recorded in the block. One set of
     * frames serves every child the same blob goes to: the first spawn copies,
     * later spawns map the same frames read-only -- a copy per spawner was the
     * cost that filled the allocator's untyped table when the second spawner
     * arrived (specs/services.md). */
    uint64_t binaries_address = 0;
    uintptr_t const after_device =
        align_up(static_cast<uintptr_t>(devices_end) + request.device_bytes, kPage);
    uintptr_t const binaries_end = [&] {
        if (request.binaries == nullptr || request.binaries_bytes == 0) {
            return after_device;
        }
        uint64_t const pages = (request.binaries_bytes + kPage - 1) / kPage;
        if (shared_binaries_frames_ != nullptr) {
            if (shared_binaries_ != request.binaries ||
                shared_binaries_pages_ < pages) {
                detail_ = "a second binaries blob is not the one the shared copy holds";
                return uintptr_t{0};
            }
            /* A frame cap remembers the one address space it is mapped in
             * (kernel/src/arch/riscv/kernel/vspace.c:867-875 refuses a
             * second), and a *copy* of it is born pristine
             * (kernel/src/arch/riscv/object/objecttype.c:34-37) -- so each
             * later child gets a copy of the frame, mapped read-only. The
             * sharing costs a slot per page, not the memory again. */
            for (uint64_t page = 0; page < pages; ++page) {
                seL4_CPtr const copy = allocator_.alloc_slot();
                if (copy == 0) {
                    detail_ = "no slot for a shared initrd frame's copy";
                    return uintptr_t{0};
                }
                seL4_Error const copied =
                    seL4_CNode_Copy(source_root_, copy, source_depth_, source_root_,
                                    shared_binaries_frames_[page], source_depth_,
                                    seL4_AllRights);
                if (copied != seL4_NoError) {
                    error_ = copied;
                    detail_ = "a shared initrd frame could not be copied";
                    return uintptr_t{0};
                }
                seL4_Error mapped = seL4_NoError;
                if (!vspace.map_page(after_device + static_cast<uintptr_t>(page) * kPage,
                                     copy, false, account, &mapped)) {
                    error_ = mapped;
                    detail_ = "a shared initrd frame could not be mapped into the child";
                    return uintptr_t{0};
                }
            }
            binaries_address = after_device;
            return after_device + pages * kPage;
        }
        auto *frames = static_cast<seL4_CPtr *>(
            arena_.allocate(sizeof(seL4_CPtr) * (pages != 0 ? pages : 1)));
        if (frames == nullptr) {
            detail_ = "no room to keep the shared initrd's frames";
            return uintptr_t{0};
        }
        if (!vspace.populate(after_device, static_cast<unsigned>(pages), request.binaries,
                             request.binaries_bytes, 0, false, account, nullptr, &why,
                             frames)) {
            detail_ = why;
            return uintptr_t{0};
        }
        shared_binaries_ = request.binaries;
        shared_binaries_frames_ = frames;
        shared_binaries_pages_ = static_cast<uint32_t>(pages);
        binaries_address = after_device;
        return after_device + pages * kPage;
    }();
    if (binaries_end == 0) {
        return fail("the initrd copy the child was to be given could not be mapped");
    }

    /* Where a service's own memory goes, chosen the way a device's is: the spawner knows the
     * layout of the address space it is filling, and the child cannot map for itself. It goes
     * *past the device window and the binaries*, which are mapped at `after_device` -- the same
     * expression would put it on top of them, and the kernel refuses a second mapping at one
     * address. */
    uintptr_t const memory_at = align_up(binaries_end, kPage);
    /* The shared window a data port serves through goes right after the memory,
     * where the spawner knows both ends, and the child's own VSpace window
     * starts past it -- a service that maps for itself still has its port's
     * window placed for it, because the window is part of the port. A window
     * of mega pages is aligned up to its frame size: a 2 MiB mapping needs a
     * page-table slot of its own level, which the address it lands at decides. */
    uintptr_t const window_frame_bytes =
        request.window_frame != 0 ? (1ul << request.window_page_bits) : kPage;
    if (request.window_page_bits != seL4_PageBits &&
        request.window_page_bits != seL4_LargePageBits) {
        return fail("a shared window's frames are 4 KiB pages or 2 MiB mega pages");
    }
    uintptr_t const shared_window_at =
        align_up(memory_at + request.memory_bytes, window_frame_bytes);
    /* Everything above the memory and the shared window is the child's, when it
     * is trusted with its own VSpace root: addresses cost nothing, so the
     * window is generous, and the spawner -- not the child -- is what chose
     * it. */
    constexpr uint64_t kWindowBytes = 1ull << 30;
    uint64_t const window_base =
        request.give_vspace ? shared_window_at + request.window_bytes : 0;

    auto *device_cap_entries =
        static_cast<bootstrap::DeviceCapEntry *>(arena_.allocate(
            sizeof(bootstrap::DeviceCapEntry) * (request.device_grant_count + 1)));
    if (device_cap_entries == nullptr) {
        return fail("no memory for the bootstrap block's device list");
    }
    for (uint32_t i = 0; i < request.device_grant_count; ++i) {
        device_cap_entries[i] = bootstrap::DeviceCapEntry{request.device_grants[i].physical,
                                                          request.device_grants[i].bytes,
                                                          device_slot_base + i};
    }

    /* The window exists in the block only when the caller gave frames for it:
     * an address of zero is how "no shared window" is written down. */
    uint64_t const shared_window_address =
        request.window_frame != 0 && request.window_bytes > 0 ? shared_window_at : 0;
    bootstrap::Contents const contents{
        request.name,       request.name_length,   request.account, request.account_length,
        request.cwd,        request.cwd_length,
        port_entries,       port_count,            devices_address, request.devices_bytes,
        device_address,     request.device_bytes,  request.device_physical,
        request.untyped_physical, request.untyped_bits, memory_at,
        binaries_address,   request.binaries_bytes, window_base,    kWindowBytes,
        device_cap_entries, request.device_grant_count,
        shared_window_address, request.window_bytes, request.window_physical,
    };
    if (bootstrap::write(block_storage, kBlockBytes, contents) == nullptr) {
        return fail("the bootstrap block does not fit its page");
    }
    if (!vspace.populate(block_at, 1, block_storage, kBlockBytes, 0, false, account, nullptr,
                         &why)) {
        detail_ = why;
        return fail("the bootstrap block could not be mapped");
    }

    /* The memory follows the device window it was placed after. */
    if (request.memory_frame != 0 && request.memory_bytes > 0) {
        uint32_t const pages = request.memory_bytes / static_cast<uint32_t>(kPage);
        for (uint32_t i = 0; i < pages; ++i) {
            seL4_Error mapped = seL4_NoError;
            if (!vspace.map_page(memory_at + i * kPage, request.memory_frame + i, true, account,
                                 &mapped)) {
                return fail("the memory a service asked for could not be mapped into it");
            }
        }
    }
    /* And the shared window follows the memory, the same shape: frames the
     * caller carved, sitting in consecutive slots, mapped -- not given -- so
     * both sides of the port hold the same pages at their own spawn times.
     * A frame is `window_page_bits` wide -- 4 KiB for a window a port's
     * clients copy through, 2 MiB for one a device scans out of. */
    if (shared_window_address != 0) {
        if ((request.window_bytes % window_frame_bytes) != 0) {
            return fail("a shared window that is not a whole number of frames");
        }
        uint32_t const frames =
            request.window_bytes / static_cast<uint32_t>(window_frame_bytes);
        for (uint32_t i = 0; i < frames; ++i) {
            seL4_Error mapped = seL4_NoError;
            if (!vspace.map_page(shared_window_at + i * window_frame_bytes,
                                 request.window_frame + i, true, account, &mapped,
                                 request.window_page_bits)) {
                return fail("the shared window a port serves through could not be mapped "
                            "into the child");
            }
        }
    }

    /* The IPC buffer's frame capability is part of the TCB configuration, so it
     * comes back out of the mapping. */
    seL4_CPtr ipc_frame = 0;
    if (!vspace.populate(ipc_at, 1, nullptr, 0, 0, true, account, &ipc_frame, &why)) {
        detail_ = why;
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
    if (!vspace.populate(stack_lo, stack_pages, stack, stack_bytes, 0, true, account, nullptr,
                         &why)) {
        detail_ = why;
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
        if (grant.capability == 0 ||
            (grant.move ? !install_moved(process.cspace, grant.slot, grant.capability)
             : grant.copy
                 ? !install_copied(process.cspace, grant.slot, grant.capability)
                 : !install(process.cspace, grant.slot, grant.capability, grant.rights,
                            grant.badge))) {
            return fail("a port could not be installed into the child");
        }
    }
    /* The vspace grant and the device grants sit where the block said they would:
     * the slots were chosen before the block was written, and an install uses the
     * slot it announced. */
    if (request.give_vspace &&
        !install(process.cspace, vspace_slot, vspace.root(), seL4_AllRights, 0)) {
        return fail("the child's own address space could not be given to it");
    }
    for (uint32_t i = 0; i < request.device_grant_count; ++i) {
        DeviceGrant const &grant = request.device_grants[i];
        if (grant.frame == 0 ||
            !install(process.cspace, device_slot_base + i, grant.frame, seL4_AllRights, 0)) {
            return fail("a device frame could not be given to the child");
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
        seL4_CNode_CapData_new(0, seL4_WordBits - bootstrap::kCNodeBits).words[0];
    error = seL4_TCB_Configure(process.tcb, bootstrap::kSlotFaultEndpoint, process.cspace,
                               cspace_guard, vspace.root(), 0, ipc_at, ipc_frame);
    if (error != seL4_NoError) {
        return fail("the child's TCB could not be configured");
    }
    /* A fresh TCB's maximum controlled priority is zero
     * (kernel/src/object/objecttype.c:525 sets only what differs from zero), and
     * SetPriority refuses a priority above the MCP the authority cap may confer
     * (kernel/src/object/tcb.c:1236) -- so the child's own priority would be
     * unsettable by anyone but the root task. Raise its MCP first. */
    error = seL4_TCB_SetMCPriority(process.tcb, seL4_CapInitThreadTCB, request.priority);
    if (error != seL4_NoError) {
        return fail("the child's maximum priority could not be set");
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
    process.vspace_root = vspace.root();
    process.mapped_end = shared_window_at + request.window_bytes;
    return true;
}

}  // namespace aegir::spawn
