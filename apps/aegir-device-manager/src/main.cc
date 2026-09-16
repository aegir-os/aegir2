/*
 * aegir-device-manager: the service that knows what the machine is.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Director gives it the machine's own description of itself -- the device tree,
 * mapped into its address space -- and reading that is its whole job to begin
 * with (specs/services.md). Nothing here probes hardware, and nothing here was
 * told about hardware by whoever spawned it: a device manager that had to be told
 * what the machine has would be a device manager that could not be given a
 * machine it had not seen before.
 *
 * The next thing it grows is the bus -> device id -> service map and the drivers
 * it launches from it. What it does today is read, report, and say it is ready, so
 * the boot set has something that owns the machine's description rather than
 * having director repeat it.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/devtree.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

void write_line(char const *label, char const *text) noexcept
{
    aegir::debug_write("      ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

/** What the tree says, bus by bus. A "bus" is the device's own `compatible`
 *  string for now: grouping devices by the transport they sit on is the job of
 *  the map this service is going to hold, not of a visitor. */
class BusReport : public aegir::devtree::Tree::Visitor {
public:
    bool device(aegir::devtree::Device const &device) override {
        ++total;
        if (!device.has_region) {
            return true;
        }
        ++with_region;
        /* The first `compatible` string is NUL-terminated in the blob, and the
         * reader checked that it is inside the property it came from. */
        aegir::debug_write("        ");
        aegir::debug_write(device.compatible);
        aegir::debug_write(" ");
        aegir::debug_write_hex(device.base);
        if (device.base == mine) {
            aegir::debug_write("  <- mine");
        }
        if (device.has_interrupt) {
            aegir::debug_write(" irq ");
            aegir::debug_write_unsigned(device.interrupt);
        }
        aegir::debug_write("\n");
        return true;
    }

    unsigned total = 0;
    unsigned with_region = 0;
    uint64_t mine = 0;
};

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    /* Tell the logger we are here. A service that cannot say what it is doing is
     * a service nobody can supervise (specs/services.md), so this is the first
     * thing every service in the boot set does. */
    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    } else {
        write_line("log.main port", "not given");
    }

    write_line("device manager", "reading the machine's device tree");

    uint64_t address = 0;
    uint32_t bytes = 0;
    if (!aegir::bootstrap::devices(&address, &bytes)) {
        write_line("FAIL", "no device tree was given to me");
        return 0;
    }

    aegir::devtree::Tree tree;
    if (!tree.adopt(reinterpret_cast<void const *>(address), bytes)) {
        write_line("FAIL", "the blob I was given is not a device tree");
        return 0;
    }

    /* The device this service is for, if it was given one. A driver's first line is
     * reading its device's identity: the magic says a real transport is there, and the
     * device id says whether anything is behind it (virtio 1.x, 4.2.2). Read before
     * the walk, because the map below marks which device is ours -- and identical
     * transports are told apart only by where they are. */
    uint64_t device_address = 0;
    uint32_t device_bytes = 0;
    uint64_t device_physical = 0;
    if (!aegir::bootstrap::device(&device_address, &device_bytes, &device_physical)) {
        write_line("my device", "none was given");
    } else {
        auto *registers = reinterpret_cast<volatile uint32_t *>(device_address);
        uint32_t const magic = registers[0x00 / 4];
        uint32_t const device_id = registers[0x08 / 4];
        aegir::debug_write("      my device at ");
        aegir::debug_write_hex(device_address);
        aegir::debug_write(": magic ");
        aegir::debug_write_hex(magic);
        aegir::debug_write(", device id ");
        aegir::debug_write_unsigned(device_id);
        aegir::debug_write(magic == 0x74726976u ? "  (virtio: the magic reads)\n"
                                               : "  (not a virtio transport)\n");
    }

    BusReport report;
    report.mine = device_physical;
    if (!tree.walk(report)) {
        write_line("FAIL", "the device tree could not be read");
        return 0;
    }

    aegir::debug_write("      tree: ");
    aegir::debug_write_unsigned(report.total);
    aegir::debug_write(" devices, ");
    aegir::debug_write_unsigned(report.with_region);
    aegir::debug_write(" with a register window\n");

    /* The authority director delegates to a service that will start processes of its
     * own: an ASID pool to take an address space id from, and an untyped to retype a
     * root page table out of (specs/authority.md). This is the first thing in Aegir a
     * service does with delegated authority rather than with what it was given to
     * read, so it says what happened. */
    uint64_t pool_slot = 0;
    uint64_t untyped_slot = 0;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    uint32_t named = 0;
    for (uint32_t e = 0; block != nullptr && e < block->entry_count; ++e) {
        if (block->entries[e].kind == aegir::bootstrap::EntryKind::Capability) {
            ++named;
        }
    }
    if (!aegir::bootstrap::capability("untyped", 7, &untyped_slot) ||
        !aegir::bootstrap::capability("asid-pool", 9, &pool_slot)) {
        write_line("authority", "no pool and no memory were given");
    } else {
        /* The first slot after the ones the block names is ours to use: the block is
         * the map of what was given, and the layout past it is nobody else's business
         * (specs/services.md). */
        seL4_CPtr const table = aegir::bootstrap::kSlotFirstDeclared + named;
        /* `node_depth == 0` is the kernel's way of being told that the destination IS
         * the capability passed as the root, and the slot inside it is the offset
         * (kernel/src/object/untyped.c, `decodeUntypedInvocation`: with a non-zero
         * depth it looks the destination *up* in that CNode instead, and a guard it
         * does not match is "Invalid destination address"). */
        seL4_Error const retyped =
            seL4_Untyped_Retype(untyped_slot, seL4_RISCV_PageTableObject, seL4_PageTableBits,
                                seL4_CapInitThreadCNode, 0, 0, table, 1);
        if (retyped != seL4_NoError) {
            write_line("FAIL", "the untyped could not be made into a page table");
        } else {
            seL4_Error const assigned = seL4_RISCV_ASIDPool_Assign(pool_slot, table);
            if (assigned != seL4_NoError) {
                write_line("FAIL", "no address space id from the pool");
            } else {
                aegir::debug_write("      my own address space: page table at cap ");
                aegir::debug_write_unsigned(table);
                aegir::debug_write(", with an address space id of my own\n");
            }
        }
    }

    /* Ready: whoever spawned us can carry on, and the supervisor can tell
     * everyone else apart from us (specs/director.md). */
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("device manager", "ready");

    /* Nothing to serve yet. A service with no port of its own has nothing to wait
     * on, and the map and the drivers it launches are what will give it one, so
     * until then it stops rather than spins at somebody else's priority. */
    aegir::debug_write("      device manager: the map and the drivers come next\n");
    aegir::halt();
}
