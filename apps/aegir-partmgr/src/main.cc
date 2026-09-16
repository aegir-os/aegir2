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
 * first), and name the partitions (BD0Part0, BD0Part1, ...). Starting the
 * filesystem service each partition's type calls for, with a range grant
 * rather than the whole device, is the next step (specs/services.md).
 */

#include "gpt.h"

#include <aegir/block.h>
#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

/* The allocator and the scratch window: static for the same reason the device
 * manager's are -- an Allocator's untyped table is tens of kilobytes, and a
 * spawned process's stack is two pages (specs/userland.md). */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

/* A block port arrives under the driver's instance name, and instance names of
 * block drivers begin this way (specs/services.md). */
bool name_is_block_port(char const *name, uint32_t length)
{
    return length >= 4 && name[0] == 'b' && name[1] == 'l' && name[2] == 'k' &&
           name[3] == '.';
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

    /* The windows arrive as frame capabilities grouped per port in the ports'
     * own order, pages ascending -- the convention the device manager grants
     * by, because a capability carries no name to pair by. */
    if (port_count == 0 || grant_count == 0 || grant_count % port_count != 0) {
        aegir::debug_write("      partition manager: ");
        aegir::debug_write_unsigned(port_count);
        aegir::debug_write(" block ports, ");
        aegir::debug_write_unsigned(grant_count);
        aegir::debug_write(" window frames -- nothing to pair up\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    uint32_t const pages_per_window = grant_count / port_count;

    aegir::debug_write("      partition manager: ");
    aegir::debug_write_unsigned(port_count);
    aegir::debug_write(" block ports, ");
    aegir::debug_write_unsigned(pages_per_window);
    aegir::debug_write(" window pages each\n");

    uint32_t port_index = 0;
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
            uint32_t const grant = port_index * pages_per_window + p;
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
                uint64_t const table_bytes =
                    static_cast<uint64_t>(entry_count) * entry_bytes;
                uint32_t const table_sectors =
                    static_cast<uint32_t>((table_bytes + 511) / 512);
                aegir::ipc::Reply const table = port.call(
                    aegir::block::kMethodRead,
                    aegir::block::pack_read(entries_lba, table_sectors));
                if (table.error != 0 || table.word != table_sectors) {
                    aegir::debug_write("      FAIL partition manager: the entries would not read\n");
                } else {
                    for (uint32_t i = 0; i < entry_count; ++i) {
                        aegir::gpt::Partition partition;
                        if (!aegir::gpt::entry(shared + static_cast<uint64_t>(i) * entry_bytes,
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
                        aegir::debug_write("\n");
                    }
                }
            }
        }

        /* Hand the window's pages back in reverse: the scratch recycles the
         * most recent mapping, so the rhythm is strictly last-in-first-out. */
        for (uint32_t p = pages_per_window; p > 0; --p) {
            uint64_t grant_slot = 0;
            static_cast<void>(aegir::bootstrap::device_capability(
                port_index * pages_per_window + p - 1, nullptr, nullptr, &grant_slot));
            g_scratch.unmap(static_cast<seL4_CPtr>(grant_slot));
        }
        ++port_index;
    }

    aegir::debug_write("      partition manager: ready\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
