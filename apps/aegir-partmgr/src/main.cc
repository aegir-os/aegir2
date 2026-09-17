/*
 * aegir-partmgr: the partition manager.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The device manager starts this service once block drivers answer, and hands
 * it the caller half of their ports and the frames of their shared windows.
 * Its job is the filesystem-agnostic step between a block device and a
 * filesystem: ask each driver who it is, read the partition table (GPT
 * first), and name the partitions (BD0Part0, BD0Part1, ...). It starts the
 * filesystem service each partition's type calls for, with a range grant
 * rather than the whole device, and -- because a filesystem it starts is not
 * in the manifest and cannot declare a need -- it speaks to the VFS on the
 * filesystem's behalf: it hands the filesystem a port of its own to announce
 * the volume's label on, and registers the label together with the volume
 * port's caller half it kept at the spawn (specs/vfs.md).
 */

#include "gpt.h"

#include <aegir/block.h>
#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/vspace.h>
#include <aegir/nmspace.h>
#include <aegir/partman.h>
#include <aegir/registry.h>
#include <aegir/spawn/initrd.h>
#include <aegir/spawn/process.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

/* The allocator and the scratch window: static for the same reason the device
 * manager's are -- an Allocator's untyped table is tens of kilobytes, and a
 * spawned process's stack is two pages (specs/userland.md). */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);
aegir::mem::Account g_account{"partmgr", 0, 0, 0};

/* A block port arrives under the driver's instance name, and instance names of
 * block drivers begin this way (specs/services.md). */
bool name_is_block_port(char const *name, uint32_t length)
{
    return length >= 4 && name[0] == 'b' && name[1] == 'l' && name[2] == 'k' &&
           name[3] == '.';
}

/* A partition the table walk found, remembered for the spawn phase. The two
 * phases are separate because they cannot overlap: a filesystem service
 * serves through the same window frames the walk reads through -- one
 * physical buffer per device, and every consumer's caps name those same
 * frames -- so the walk's last read comes before the first spawn, and the
 * "serve the one announce between spawn and ready" rhythm below never
 * touches the window at all. */
struct Pending {
    seL4_CPtr port;        /* the block port's caller half */
    char device_name[8];   /* Identify's name field */
    uint32_t device_name_length;
    uint32_t partition;    /* its index in the table */
    uint64_t first_lba;
    uint64_t sector_count;
    uint32_t port_index;   /* which window's frames its child maps with */
    uint32_t boot;         /* the Aegir system type GUID said so */
    Pending *next;
};

/* A copy set of a window's frames, minted from one of the granted groups into
 * slots of our own, for one filesystem service to be mapped with. The group
 * it comes from is never mapped by anyone, so the copies arrive with no ASID
 * -- a frame's first mapping pins its ASID into the capability, and a set
 * that stayed unmapped mints mappable copies for ever
 * (kernel/src/arch/riscv/kernel/vspace.c:869-878). */
seL4_CPtr mint_window_set(uint32_t first_grant, uint32_t pages) noexcept
{
    seL4_CPtr base = 0;
    for (uint32_t p = 0; p < pages; ++p) {
        uint64_t source = 0;
        if (!aegir::bootstrap::device_capability(first_grant + p, nullptr, nullptr,
                                                 &source)) {
            return 0;
        }
        seL4_CPtr const slot = g_objects.alloc_slot();
        if (slot == 0 ||
            seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, slot,
                            aegir::bootstrap::kCNodeBits,
                            aegir::bootstrap::kSlotOwnCNode,
                            static_cast<seL4_CPtr>(source), aegir::bootstrap::kCNodeBits,
                            seL4_AllRights, 0) != seL4_NoError) {
            return 0;
        }
        if (p == 0) {
            base = slot;
        }
    }
    return base;
}

namespace {

void append_text(char *out, uint32_t *at, char const *text, uint32_t length) noexcept
{
    for (uint32_t i = 0; i < length; ++i) {
        out[(*at)++] = text[i];
    }
}

void append_number(char *out, uint32_t *at, uint64_t value) noexcept
{
    char digits[20];
    uint32_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0) {
        out[(*at)++] = digits[--count];
    }
}

}  // namespace

/* Start the filesystem service for one partition: the caller half of the
 * block port badged with who it is, the window mapped at spawn time by us,
 * and the helper's image as bytes. Badges count from 512: our spawner's
 * children are 256+n, and a spawning service's children live in a range of
 * their own until the badge space is a designed thing (specs/services.md).
 *
 * Two ports of our own making go with it: the owner half of its volume port
 * ("vol") -- whose caller half we keep, unbadged, because the VFS badges
 * each resolver's own copy of it -- and the caller half of our announce
 * port, which it tells us the volume's label on (specs/vfs.md). The label
 * is the filesystem's own to know, so registration is a conversation: the
 * child announces, we register the label with the VFS together with the
 * caller half we kept, and the child gets the name the volume actually got.
 * The child blocks in its announce until we answer, so the answer is served
 * here, between the spawn and the wait for its ready. */
void start_filesystem(aegir::spawn::Spawner &spawner, seL4_CPtr spawn_log,
                      aegir::ipc::Consumer const &nmspace, seL4_CPtr announce,
                      char const *device_name, uint32_t device_name_length,
                      uint32_t partition, uint64_t first_lba, uint64_t sector_count,
                      uint32_t boot,
                      seL4_CPtr block_port, uint32_t children_grant,
                      uint64_t window_physical, uint32_t window_pages,
                      void const *fs_image, uint32_t fs_image_bytes,
                      uint64_t badge) noexcept
{
    /* Its name: the driver's name for the device, the partition's index, and
     * the kind of service -- fat.BD0Part0. Bounded by the name's own format:
     * the device name is 8 by the block protocol, the index is a 32-bit
     * number. */
    char name[4 + 8 + 4 + 10];
    uint32_t name_length = 0;
    char const *kind = "fat.";
    for (uint32_t i = 0; kind[i] != '\0'; ++i) {
        name[name_length++] = kind[i];
    }
    for (uint32_t i = 0; i < device_name_length; ++i) {
        name[name_length++] = device_name[i];
    }
    char const *part = "Part";
    for (uint32_t i = 0; part[i] != '\0'; ++i) {
        name[name_length++] = part[i];
    }
    char digits[10];
    uint32_t digit_count = 0;
    for (uint32_t n = partition;; n /= 10) {
        digits[digit_count++] = static_cast<char>('0' + n % 10);
        if (n < 10) {
            break;
        }
    }
    while (digit_count > 0) {
        name[name_length++] = digits[--digit_count];
    }

    /* The range grant, as a descriptor row the child parses (the format is
     * libs/aegir-descriptor's): the partition's first sector and length on
     * the device, the volume's public name, and whether the volume takes
     * writes -- the same statement the registration below makes to the VFS,
     * one source. A grant the child reads
     * rather than authority the kernel checks -- clamping by badge is the
     * driver's business, and comes with the first writer
     * (specs/services.md). The buffer must live until spawn has copied it,
     * which this scope guarantees; its size is the format's own bound. */
    char range[96];
    uint32_t range_length = 0;
    append_text(range, &range_length, "first=", 6);
    append_number(range, &range_length, first_lba);
    append_text(range, &range_length, " sectors=", 9);
    append_number(range, &range_length, sector_count);
    append_text(range, &range_length, " name=", 6);
    /* The volume's name is the child's without the kind: BD0Part0. */
    append_text(range, &range_length, name + 4, name_length - 4);
    append_text(range, &range_length, " writable=1", 11);
    range[range_length++] = '\n';

    /* The handle table's room (specs/vfs.md): a page the child's open
     * files live in, because a filesystem handed no memory has nowhere to
     * remember who has what open. The bound is the grant -- the clamp
     * table's shape, one service back -- and reaching it is a loud
     * refusal. */
    seL4_CPtr const window = mint_window_set(children_grant, window_pages);
    aegir::mem::Account child_account{"fs", 0, 0, 0};
    constexpr uint32_t kFsMemoryBits = 12; /* one page */
    seL4_Error memory_error = seL4_NoError;
    uint64_t memory_physical = 0;
    seL4_CPtr const memory_untyped =
        g_objects.carve_untyped(kFsMemoryBits, child_account, &memory_error,
                                &memory_physical);
    seL4_CPtr memory_frame = 0;
    if (memory_untyped != 0) {
        memory_frame = g_objects.carve_page(memory_untyped, child_account, &memory_error);
    }
    seL4_Error fault_error = seL4_NoError;
    seL4_CPtr const fault = g_objects.alloc_object(seL4_EndpointObject, seL4_EndpointBits,
                                                   child_account, &fault_error);
    seL4_Error volume_error = seL4_NoError;
    seL4_CPtr const volume = g_objects.alloc_object(seL4_EndpointObject,
                                                    seL4_EndpointBits, child_account,
                                                    &volume_error);
    seL4_CPtr const volume_caller = g_objects.alloc_slot();
    bool const caller_minted =
        volume != 0 && volume_caller != 0 &&
        seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, volume_caller,
                        aegir::bootstrap::kCNodeBits, aegir::bootstrap::kSlotOwnCNode,
                        volume, aegir::bootstrap::kCNodeBits,
                        seL4_CapRights_new(1, 0, 0, 1), 0) == seL4_NoError;
    if (window == 0 || fault == 0 || volume == 0 || !caller_minted || memory_frame == 0) {
        aegir::debug_write("      FAIL starting ");
        aegir::debug_write(name, name_length);
        aegir::debug_write(": no window set, fault endpoint, volume port, or memory\n");
        return;
    }

    aegir::spawn::PortGrant const ports[] = {
        {aegir::log::kPortName, aegir::log::kPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared, spawn_log, seL4_CapRights_new(1, 0, 0, 1),
         badge, 0},
        /* The block port, caller half, badged: the driver learns which
         * filesystem is asking, which is what a range grant will one day
         * clamp by (specs/services.md). */
        {"blk", 3, aegir::bootstrap::kSlotFirstDeclared + 1, block_port,
         seL4_CapRights_new(1, 0, 0, 1), badge, 0},
        /* Its volume port, receiving half only: a port you may not receive
         * on is not yours, and this one is. */
        {"vol", 3, aegir::bootstrap::kSlotFirstDeclared + 2, volume, seL4_CanRead,
         0, 0},
        /* Our announce port, calling half: where it tells us the volume's
         * label. No badge and no mark -- we serve the one announce between
         * this spawn and the wait for its ready, so there is nothing to
         * tell apart. */
        {aegir::partman::kPortName, aegir::partman::kPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared + 3, announce,
         seL4_CapRights_new(1, 0, 0, 1), 0, 0},
    };
    aegir::spawn::Request request{};
    request.name = name;
    request.name_length = name_length;
    request.binary = name; /* unused: the image comes as bytes */
    request.binary_length = name_length;
    request.binary_image = fs_image;
    request.binary_image_bytes = fs_image_bytes;
    request.account = "system";
    request.account_length = 6;
    request.priority = seL4_MaxPrio - 1;
    request.ports = ports;
    request.port_count = 4;
    request.devices = range;
    request.devices_bytes = range_length;
    request.window_frame = window;
    request.window_bytes = window_pages * 4096u;
    request.window_physical = window_physical;
    /* The handle-table page: where it lands and how big it is travel in the
     * block's untyped entry, the way a driver's memory does (the child
     * serves no DMA, so the physical base is information, not plumbing). */
    request.memory_frame = memory_frame;
    request.memory_bytes = 1u << kFsMemoryBits;
    request.untyped_physical = memory_physical;
    request.untyped_bits = kFsMemoryBits;
    request.fault_endpoint = fault;
    request.badge = badge;

    /* The range, enforced before the child exists to hold the badge: the
     * driver learns which sectors this badge may read, and only the badge-0
     * caller -- this manager -- may tell it (aegir/block.h). Then the proof,
     * asked with the child's own badge: sector 0 is no GPT partition's to
     * read, so a clamp that holds refuses it. */
    uint64_t const clamp_out[3] = {badge, first_lba, sector_count};
    uint64_t clamp_answer = 0;
    aegir::ipc::Consumer const clamp_port(block_port);
    aegir::ipc::WordsReply const clamped = clamp_port.call_words(
        aegir::block::kMethodClamp, clamp_out, 3, &clamp_answer, 1);
    bool holds = false;
    seL4_CPtr const probe = g_objects.alloc_slot();
    if (probe != 0 &&
        seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, probe,
                        aegir::bootstrap::kCNodeBits, aegir::bootstrap::kSlotOwnCNode,
                        block_port, aegir::bootstrap::kCNodeBits,
                        seL4_CapRights_new(1, 0, 0, 1), badge) == seL4_NoError) {
        aegir::ipc::Consumer const probe_port(probe);
        aegir::ipc::Reply const refused =
            probe_port.call(aegir::block::kMethodRead, aegir::block::pack_read(0, 1));
        holds = refused.error == 0 && refused.word == 0;
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, probe,
                          aegir::bootstrap::kCNodeBits);
    }
    if (clamped.error != 0 || clamped.count != 1 || clamp_answer != 1 || !holds) {
        aegir::debug_write("      FAIL starting ");
        aegir::debug_write(name, name_length);
        aegir::debug_write(": the range clamp would not record or does not hold\n");
        return;
    }
    aegir::debug_write("      clamp: badge ");
    aegir::debug_write_unsigned(badge);
    aegir::debug_write(" refused sector 0, holds [");
    aegir::debug_write_unsigned(first_lba);
    aegir::debug_write("..");
    aegir::debug_write_unsigned(first_lba + sector_count - 1);
    aegir::debug_write("]\n");

    aegir::spawn::Process process{};
    if (!spawner.spawn(request, child_account, process)) {
        aegir::debug_write("      FAIL spawning ");
        aegir::debug_write(name, name_length);
        aegir::debug_write(": ");
        aegir::debug_write(spawner.problem());
        if (spawner.detail()[0] != '\0') {
            aegir::debug_write(" (");
            aegir::debug_write(spawner.detail());
            aegir::debug_write(", seL4 error ");
            aegir::debug_write_unsigned(spawner.error());
            aegir::debug_write(")");
        }
        aegir::debug_write("\n");
        return;
    }
    aegir::debug_write("      spawned ");
    aegir::debug_write(name, name_length);
    aegir::debug_write(", badge ");
    aegir::debug_write_unsigned(badge);
    aegir::debug_write("\n");

    /* The announce, served before the ready: the child blocks in it until
     * we answer, and the answer is the VFS's. A child that faults first
     * leaves us waiting here, which is what waiting on its ready did
     * before. */
    aegir::ipc::Owner announce_port(announce);
    uint64_t words[aegir::ipc::kMaxWords];
    uint32_t count = 0;
    uint32_t const method =
        announce_port.receive_words(words, aegir::ipc::kMaxWords, &count, nullptr);
    char const *label = nullptr;
    uint32_t label_length = 0;
    uint64_t in[aegir::nmspace::kNameMax / 8 + 1];
    uint32_t in_count = 0;
    if (method == aegir::partman::kMethodAnnounce && nmspace.valid() &&
        aegir::nmspace::unpack_string(words, count, aegir::nmspace::kNameMax, &label,
                                      &label_length)) {
        uint64_t out[aegir::nmspace::kNameMax / 8 + 2];
        uint32_t out_words = aegir::nmspace::pack_string(out, label, label_length,
                                                         aegir::nmspace::kNameMax);
        /* Flags: not read-only -- a FAT volume takes writes, which the
         * descriptor row already told the child -- plus boot when the
         * partition's type GUID said this is the system volume
         * (specs/services.md). One statement, two hearers. */
        out[out_words++] = boot != 0 ? aegir::nmspace::kFlagBoot : 0;
        aegir::ipc::WordsReply const registered = nmspace.call_transfer(
            aegir::nmspace::kMethodRegister, out, out_words, volume_caller, in,
            aegir::nmspace::kNameMax / 8 + 1, nullptr);
        if (registered.error == 0 && registered.count != 0) {
            in_count = registered.count;
        }
    }
    /* The kernel transferred a copy; our half of the mint leaves. And an
     * empty answer is how the child learns the announce was refused. */
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, volume_caller,
                      aegir::bootstrap::kCNodeBits);
    announce_port.reply_words(in, in_count);

    seL4_Word ready_badge = 0;
    seL4_Wait(process.supervision, &ready_badge);
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block == nullptr) {
        aegir::debug_write("      FAIL partition manager: no bootstrap block\n");
        return 0;
    }

    /* The authority to map the windows it was given: the untyped is where the
     * scratch window's page tables come from, and the slots past everything
     * the block names are ours (the same adoption the device manager does,
     * specs/authority.md). */
    uint64_t untyped_slot = 0;
    uint32_t untyped_bits = 0;
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    uint32_t port_count = 0;
    uint32_t grant_count = 0;
    for (uint32_t e = 0; e < block->entry_count; ++e) {
        aegir::bootstrap::Entry const &entry = block->entries[e];
        if (entry.kind == aegir::bootstrap::EntryKind::Capability) {
            auto const *name = reinterpret_cast<char const *>(block) + entry.data_offset;
            if (entry.length == 7 && name[0] == 'u' && name[1] == 'n' && name[2] == 't' &&
                name[3] == 'y' && name[4] == 'p' && name[5] == 'e' && name[6] == 'd') {
                untyped_slot = entry.number;
                untyped_bits = entry.reserved;
            }
            if (name_is_block_port(name, entry.length)) {
                ++port_count;
            }
            if (entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            }
        } else if (entry.kind == aegir::bootstrap::EntryKind::DeviceCapability) {
            ++grant_count;
            if (entry.reserved + 1 > first_free) {
                first_free = entry.reserved + 1;
            }
        }
    }
    uint64_t untyped_physical = 0;
    uint64_t untyped_address = 0;
    static_cast<void>(aegir::bootstrap::untyped(&untyped_physical, &untyped_bits,
                                                &untyped_address));
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    bool const have_authority =
        untyped_slot != 0 &&
        aegir::bootstrap::capability("vspace", 6, &vspace_slot) &&
        aegir::bootstrap::window(&window_base, &window_bytes);
    if (!have_authority) {
        aegir::debug_write("      FAIL partition manager: no untyped, vspace or window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    if (!g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                 untyped_physical)) {
        aegir::debug_write("      FAIL partition manager: the untyped would not be remembered\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    /* The depth is zero because these are *our* slots: at depth zero the
     * destination capability of a retype *is* the CNode
     * (kernel/src/object/untyped.c). The size is the one the spawner builds
     * (kCNodeBits in libs/aegir-spawn/src/process.cc). */
    g_objects.adopt_slots(first_free, (1u << 10) - first_free, 0);
    if (!g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                         static_cast<uintptr_t>(window_base),
                         static_cast<uintptr_t>(window_base + window_bytes), &g_objects)) {
        aegir::debug_write("      FAIL partition manager: the window would not be adopted\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The windows arrive as frame capabilities in two groups per port -- ours
     * to read through, and a set reserved for the filesystem services we
     * start -- each group pages ascending, the ports in their own order. The
     * convention is the device manager's, because a capability carries no
     * name to pair by. */
    if (port_count == 0 || grant_count == 0 || grant_count % (2 * port_count) != 0) {
        aegir::debug_write("      partition manager: ");
        aegir::debug_write_unsigned(port_count);
        aegir::debug_write(" block ports, ");
        aegir::debug_write_unsigned(grant_count);
        aegir::debug_write(" window frames -- nothing to pair up\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    uint32_t const pages_per_window = grant_count / (2 * port_count);

    /* What starting a filesystem service takes: the pool its address space id
     * comes from, the delegatable log, and the helper's image as bytes -- the
     * blob the device manager handed us, because the initrd whole does not
     * fit a delegation this size (specs/services.md). */
    uint64_t spawn_log_slot = 0;
    uint64_t pool_slot = 0;
    uint64_t fs_image_address = 0;
    uint32_t fs_image_bytes = 0;
    bool const can_spawn =
        aegir::bootstrap::capability("spawn:log.main", 14, &spawn_log_slot) &&
        aegir::bootstrap::capability("asid-pool", 9, &pool_slot) &&
        aegir::bootstrap::devices(&fs_image_address, &fs_image_bytes) &&
        fs_image_bytes != 0;
    if (!can_spawn) {
        aegir::debug_write("      partition manager: no pool, delegatable log or "
                           "filesystem image -- reading tables only\n");
    }
    /* Our own CNode at its own depth: a service's own-CNode cap is a raw copy
     * with guard 0 and radix kCNodeBits, so mint sources address through it
     * (aegir/bootstrap.h). No initrd: the image arrives as bytes. */
    aegir::mem::Arena arena(g_objects, g_scratch, g_account);
    aegir::spawn::Initrd const no_initrd(nullptr, 0);
    aegir::spawn::Spawner spawner(g_objects, g_scratch, arena, no_initrd,
                                  static_cast<seL4_CPtr>(pool_slot),
                                  static_cast<seL4_CPtr>(aegir::bootstrap::kSlotOwnCNode),
                                  aegir::bootstrap::kCNodeBits);
    uint32_t fs_started = 0;

    /* What registering a started filesystem's volume takes (specs/vfs.md):
     * the namespace's caller half, and a port of our own the filesystem
     * announces its label on -- it is not in the manifest, so its port is
     * given, not declared. */
    aegir::ipc::Consumer const nmspace =
        aegir::ipc::Consumer::find(aegir::nmspace::kPortName,
                                   aegir::nmspace::kPortNameLength);
    if (!nmspace.valid()) {
        aegir::debug_write("      vfs.namespace: not given -- volumes will not register\n");
    }
    seL4_Error announce_error = seL4_NoError;
    seL4_CPtr const announce = g_objects.alloc_object(seL4_EndpointObject,
                                                      seL4_EndpointBits, g_account,
                                                      &announce_error);
    if (announce == 0) {
        aegir::debug_write("      FAIL partition manager: no announce port\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    aegir::debug_write("      partition manager: ");
    aegir::debug_write_unsigned(port_count);
    aegir::debug_write(" block ports, ");
    aegir::debug_write_unsigned(pages_per_window);
    aegir::debug_write(" window pages each\n");

    /* The map, asked rather than printed: the device manager serves its
     * registry on a port of its own, and the caller half was one of our
     * grants. It answers from the moment we start -- it serves while it
     * waits for our ready, so asking first is not the two of us waiting on
     * each other (specs/services.md). This is a client asking, the shape
     * every later consumer takes. */
    aegir::ipc::Consumer const registry =
        aegir::ipc::Consumer::find(aegir::registry::kPortName,
                                   aegir::registry::kPortNameLength);
    if (!registry.valid()) {
        aegir::debug_write("      devmgr.registry: not given\n");
    } else {
        aegir::ipc::Reply const count = registry.call(aegir::registry::kMethodCount, 0);
        if (count.error != 0) {
            aegir::debug_write("      devmgr.registry: the count call was refused\n");
        } else {
            aegir::debug_write("      devmgr.registry, asked: ");
            aegir::debug_write_unsigned(count.word);
            aegir::debug_write(count.word == 1 ? " device\n" : " devices\n");
            for (uint64_t i = 0; i < count.word; ++i) {
                uint64_t words[aegir::registry::kRowWords];
                aegir::ipc::WordsReply const answer =
                    registry.call_words(aegir::registry::kMethodDescribe, &i, 1, words,
                                        aegir::registry::kRowWords);
                if (answer.error != 0 || answer.count != aegir::registry::kRowWords) {
                    aegir::debug_write("      devmgr.registry: a describe was refused\n");
                    break;
                }
                auto const *row = reinterpret_cast<aegir::registry::Row const *>(words);
                aegir::debug_write("        [");
                aegir::debug_write_unsigned(i);
                aegir::debug_write("] ");
                aegir::debug_write(row->instance[0] != '\0' ? row->instance : "(unbound)");
                aegir::debug_write(": ");
                aegir::debug_write(row->compatible);
                aegir::debug_write(" at ");
                aegir::debug_write_hex(row->base);
                if (row->irq != 0) {
                    aegir::debug_write(", irq ");
                    aegir::debug_write_unsigned(row->irq);
                }
                aegir::debug_write(", window ");
                aegir::debug_write_unsigned(row->window_bits);
                aegir::debug_write(" bits");
                if (row->bound != 0) {
                    aegir::debug_write(", driven by ");
                    aegir::debug_write(row->binary);
                }
                aegir::debug_write("\n");
            }
        }
    }

    uint32_t port_index = 0;
    Pending *pendings = nullptr;
    for (uint32_t e = 0; e < block->entry_count; ++e) {
        aegir::bootstrap::Entry const &entry = block->entries[e];
        if (entry.kind != aegir::bootstrap::EntryKind::Capability) {
            continue;
        }
        auto const *entry_name = reinterpret_cast<char const *>(block) + entry.data_offset;
        if (!name_is_block_port(entry_name, entry.length)) {
            continue;
        }
        /* The window this port's reads move data through: mapped here,
         * because the driver and this service are different address spaces
         * and a frame cap serves one of them -- the grant was minted from a
         * set no one had mapped, which is what makes this legal
         * (kernel/src/arch/riscv/kernel/vspace.c:869-878). */
        uint8_t *shared = nullptr;
        bool mapped = true;
        for (uint32_t p = 0; p < pages_per_window; ++p) {
            uint64_t grant_physical = 0;
            uint32_t grant_bytes = 0;
            uint64_t grant_slot = 0;
            /* Our group of this port's frames is the first of the two. */
            uint32_t const grant = port_index * 2 * pages_per_window + p;
            if (!aegir::bootstrap::device_capability(grant, &grant_physical, &grant_bytes,
                                                     &grant_slot)) {
                mapped = false;
                break;
            }
            void *page = g_scratch.map(static_cast<seL4_CPtr>(grant_slot));
            if (page == nullptr) {
                mapped = false;
                break;
            }
            if (p == 0) {
                shared = static_cast<uint8_t *>(page);
            }
        }
        if (!mapped) {
            aegir::debug_write("      FAIL partition manager: a window would not map\n");
            break;
        }

        aegir::ipc::Consumer port(static_cast<seL4_CPtr>(entry.number));
        aegir::ipc::Reply const identity = port.call(aegir::block::kMethodIdentify, 0);
        if (identity.error != 0 || identity.word != sizeof(aegir::block::Identify)) {
            aegir::debug_write("      FAIL partition manager: a block port would not identify\n");
            break;
        }
        auto const *who = reinterpret_cast<aegir::block::Identify const *>(shared);
        /* The walk's reads clobber the window, so the one number it still
         * needs from the identify answer is taken now. */
        uint32_t const device_window_sectors = who->window_sectors;
        char device_name[sizeof(who->name)];
        uint32_t device_name_length = 0;
        while (device_name_length < sizeof(who->name) &&
               who->name[device_name_length] != '\0') {
            device_name[device_name_length] = who->name[device_name_length];
            ++device_name_length;
        }
        aegir::debug_write("      ");
        aegir::debug_write(device_name, device_name_length);
        aegir::debug_write(": ");
        aegir::debug_write_unsigned(who->sector_count);
        aegir::debug_write(" sectors\n");

        /* The table walk: protective MBR at sector 0, header at sector 1,
         * entries where the header says. Every answer lands in the window,
         * which is why `who` had to be copied out of it first. */
        aegir::ipc::Reply const mbr = port.call(aegir::block::kMethodRead,
                                                aegir::block::pack_read(0, 1));
        if (mbr.error != 0 || mbr.word != 1 || !aegir::gpt::protective_mbr(shared)) {
            aegir::debug_write("      ");
            aegir::debug_write(device_name, device_name_length);
            aegir::debug_write(": no protective MBR -- not a GPT disk (reply error ");
            aegir::debug_write_unsigned(mbr.error);
            aegir::debug_write(", sectors ");
            aegir::debug_write_unsigned(mbr.word);
            aegir::debug_write(", sig ");
            aegir::debug_write_hex(shared[510]);
            aegir::debug_write(" ");
            aegir::debug_write_hex(shared[511]);
            aegir::debug_write(", entry0 type ");
            aegir::debug_write_hex(shared[450]);
            aegir::debug_write(")\n");
        } else {
            aegir::ipc::Reply const head = port.call(aegir::block::kMethodRead,
                                                     aegir::block::pack_read(1, 1));
            uint64_t entries_lba = 0;
            uint32_t entry_count = 0;
            uint32_t entry_bytes = 0;
            if (head.error != 0 || head.word != 1 ||
                !aegir::gpt::header(shared, &entries_lba, &entry_count, &entry_bytes)) {
                aegir::debug_write("      ");
                aegir::debug_write(device_name, device_name_length);
                aegir::debug_write(": a protective MBR but no GPT header\n");
            } else {
                /* The table can outlast the window: the GPT's own minimum is
                 * 128 entries but the format allows any count, so the walk
                 * reads window-sized runs. A legal entry size is 128 * 2^n --
                 * either a fraction of a sector or a whole run of them -- so
                 * a chunk holds a whole number of entries, and a chunk's
                 * first sector follows from its first entry exactly. */
                bool const entry_size_walkable =
                    entry_bytes >= 128 &&
                    (entry_bytes <= 512 ? 512 % entry_bytes == 0
                                        : entry_bytes % 512 == 0);
                uint32_t const entries_per_chunk =
                    entry_size_walkable
                        ? (entry_bytes <= 512
                               ? device_window_sectors * (512 / entry_bytes)
                               : device_window_sectors / (entry_bytes / 512))
                        : 0;
                if (entries_per_chunk == 0) {
                    aegir::debug_write(
                        "      FAIL partition manager: an entry size the walk cannot chunk\n");
                }
                bool table_broken = false;
                for (uint32_t base = 0;
                     !table_broken && entries_per_chunk > 0 && base < entry_count;
                     base += entries_per_chunk) {
                    uint32_t here = entry_count - base;
                    if (here > entries_per_chunk) {
                        here = entries_per_chunk;
                    }
                    uint32_t const chunk_sectors = static_cast<uint32_t>(
                        (static_cast<uint64_t>(here) * entry_bytes + 511) / 512);
                    aegir::ipc::Reply const chunk = port.call(
                        aegir::block::kMethodRead,
                        aegir::block::pack_read(
                            entries_lba + static_cast<uint64_t>(base) * entry_bytes / 512,
                            chunk_sectors));
                    if (chunk.error != 0 || chunk.word != chunk_sectors) {
                        aegir::debug_write(
                            "      FAIL partition manager: the entries would not read\n");
                        break;
                    }
                    for (uint32_t e = 0; e < here; ++e) {
                        uint32_t const i = base + e;
                        aegir::gpt::Partition partition;
                        if (!aegir::gpt::entry(shared + static_cast<uint64_t>(e) * entry_bytes,
                                               entry_bytes, &partition)) {
                            continue;
                        }
                        aegir::debug_write("      ");
                        aegir::debug_write(device_name, device_name_length);
                        aegir::debug_write("Part");
                        aegir::debug_write_unsigned(i);
                        aegir::debug_write(": sectors ");
                        aegir::debug_write_unsigned(partition.first_lba);
                        aegir::debug_write("..");
                        aegir::debug_write_unsigned(partition.last_lba);
                        if (partition.name_length > 0) {
                            aegir::debug_write(", \"");
                            aegir::debug_write(partition.name, partition.name_length);
                            aegir::debug_write("\"");
                        }
                        bool const system = aegir::gpt::system_volume(partition);
                        if (system) {
                            aegir::debug_write(" -- the system volume");
                        }
                        aegir::debug_write("\n");
                        /* Remembered for the spawn phase: starting the
                         * service now would put a serving child on this
                         * window while the walk is still reading through
                         * it (Pending, above). */
                        if (can_spawn) {
                            auto *pending =
                                static_cast<Pending *>(arena.allocate(sizeof(Pending)));
                            if (pending == nullptr) {
                                aegir::debug_write(
                                    "      FAIL partition manager: no memory for a partition\n");
                                continue;
                            }
                            pending->port = static_cast<seL4_CPtr>(entry.number);
                            pending->device_name_length = device_name_length;
                            for (uint32_t c = 0; c < device_name_length; ++c) {
                                pending->device_name[c] = device_name[c];
                            }
                            pending->partition = i;
                            pending->first_lba = partition.first_lba;
                            pending->sector_count =
                                partition.last_lba - partition.first_lba + 1;
                            pending->port_index = port_index;
                            pending->boot = system ? 1 : 0;
                            pending->next = pendings;
                            pendings = pending;
                        }
                    }
                }
            }
        }

        /* Hand the window's pages back in reverse: the scratch recycles the
         * most recent mapping, so the rhythm is strictly last-in-first-out. */
        for (uint32_t p = pages_per_window; p > 0; --p) {
            uint64_t grant_slot = 0;
            static_cast<void>(aegir::bootstrap::device_capability(
                port_index * 2 * pages_per_window + p - 1, nullptr, nullptr, &grant_slot));
            g_scratch.unmap(static_cast<seL4_CPtr>(grant_slot));
        }
        ++port_index;
    }

    /* The walk is done, and with it every read through a window. Now the
     * spawns: each child serves through its window's frames from the moment
     * it starts, which is exactly what the walk could not have happening
     * underneath it (Pending, above). A child's group of its port's frames
     * is the second of the two. */
    for (Pending *pending = pendings; pending != nullptr; pending = pending->next) {
        uint32_t const children_grant =
            pending->port_index * 2 * pages_per_window + pages_per_window;
        uint64_t window_physical = 0;
        static_cast<void>(aegir::bootstrap::device_capability(
            children_grant, &window_physical, nullptr, nullptr));
        start_filesystem(spawner, static_cast<seL4_CPtr>(spawn_log_slot), nmspace,
                         announce, pending->device_name, pending->device_name_length,
                         pending->partition, pending->first_lba, pending->sector_count,
                         pending->boot, pending->port, children_grant, window_physical,
                         pages_per_window,
                         reinterpret_cast<void const *>(fs_image_address), fs_image_bytes,
                         512u + fs_started);
        ++fs_started;
    }

    aegir::debug_write("      partition manager: ready\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
