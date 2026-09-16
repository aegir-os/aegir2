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
#include <aegir/mem/allocator.h>
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

namespace {
aegir::mem::Allocator g_objects(nullptr);
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
    /* Static, not local, and that is not a style choice: an Allocator carries the table
     * of untyped memory it knows about -- room for the kernel's whole list, plus the
     * halves splitting creates -- which is tens of kilobytes. The root task has a large
     * initial stack and can keep one on `main`'s; a spawned process has two pages, and
     * putting one there overflows the stack into unmapped memory (specs/userland.md). */
    uint64_t untyped_bits = 0;
    /* The first slot past everything the block names is where our own capabilities
     * may go -- and *everything* the block names: a DeviceCapability carries its
     * slot in `reserved` rather than `number`, so counting only Capability entries
     * starts the cursor on top of a frame the spawner installed (the kernel's
     * answer is seL4_DeleteFirst, "the destination slot is occupied"). */
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    for (uint32_t e = 0; block != nullptr && e < block->entry_count; ++e) {
        aegir::bootstrap::Entry const &entry = block->entries[e];
        if (entry.kind == aegir::bootstrap::EntryKind::Capability) {
            /* The size of what a capability is, when it has one. It is in the block rather
             * than asked of the kernel, because there is no invocation that reads an
             * untyped's size (specs/authority.md). */
            auto const *name = reinterpret_cast<char const *>(block) + entry.data_offset;
            if (entry.length == 7 && name[0] == 'u' && name[1] == 'n' && name[2] == 't' &&
                name[3] == 'y' && name[4] == 'p' && name[5] == 'e' && name[6] == 'd') {
                untyped_bits = entry.reserved;
            }
            if (entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            }
        } else if (entry.kind == aegir::bootstrap::EntryKind::DeviceCapability &&
                   entry.reserved + 1 > first_free) {
            first_free = entry.reserved + 1;
        }
    }
    if (!aegir::bootstrap::capability("untyped", 7, &untyped_slot) ||
        !aegir::bootstrap::capability("asid-pool", 9, &pool_slot)) {
        write_line("authority", "no pool and no memory were given");
    } else {
        /* The block's `untyped` entry says where the memory is in the machine: a
         * capability carries no address, and nothing this region becomes may be
         * named to a device without one (specs/services.md). */
        uint64_t untyped_physical = 0;
        uint32_t entry_bits = 0;
        uint64_t untyped_address = 0;
        static_cast<void>(aegir::bootstrap::untyped(&untyped_physical, &entry_bits,
                                                    &untyped_address));
        /* Through the allocator, not a raw retype: the memory and the slots this
         * service may put capabilities in were handed to it, so its allocator is
         * adopted rather than discovered -- which is what makes the spawner usable by
         * a service and not only by the root task (specs/authority.md). The depth is
         * zero because these are *our* slots: at depth zero the destination capability
         * of a retype *is* the CNode (kernel/src/object/untyped.c). */
        aegir::mem::Account me{"devicemgr", 0, 0, 0};
        seL4_CPtr table = 0;
        if (!g_objects.adopt_untyped(untyped_slot, untyped_bits, untyped_physical)) {
            write_line("FAIL", "no room to remember the memory I was given");
        } else {
            /* Every slot past the ones the block names is ours to use: the block is
             * the map of what was given, and the layout past it is nobody else's
             * business (specs/services.md). Splitting the untyped down to a page
             * table takes a slot per half it leaves behind, so one slot is not a
             * service's working set -- the rest of the CSpace is. The size is the
             * one the spawner builds (kCNodeBits in libs/aegir-spawn/src/process.cc). */
            g_objects.adopt_slots(first_free, (1u << 10) - first_free, 0);
            seL4_Error error = seL4_NoError;
            table = g_objects.alloc_object(seL4_RISCV_PageTableObject, seL4_PageTableBits, me, &error);
            if (table == 0) {
                aegir::debug_write("      FAIL the untyped could not be made into a page table (seL4 error ");
                aegir::debug_write_unsigned(static_cast<uint64_t>(error));
                aegir::debug_write(")\n");
            }
        }
        if (table != 0) {
            seL4_Error const assigned = seL4_RISCV_ASIDPool_Assign(pool_slot, table);
            if (assigned != seL4_NoError) {
                write_line("FAIL", "no address space id from the pool");
            } else {
                aegir::debug_write("      my own memory: ");
        aegir::debug_write_unsigned(untyped_bits);
        aegir::debug_write(" bits of untyped at physical ");
        aegir::debug_write_hex(untyped_physical);
        aegir::debug_write(", as the block says\n");
        aegir::debug_write("      my own address space: page table at cap ");
                aegir::debug_write_unsigned(table);
                aegir::debug_write(", with an address space id of my own\n");
            }
        }
    }

    /* What a spawning service is given besides memory: its own VSpace root with a
     * window of free addresses, a copy of the initrd to read images out of, and
     * the devices its children are for as capabilities to hand on
     * (specs/services.md). Reporting them is what proves the grant arrived the way
     * the block said it would. */
    uint64_t vspace_slot = 0;
    if (aegir::bootstrap::capability("vspace", 6, &vspace_slot)) {
        uint64_t window_base = 0;
        uint32_t window_bytes = 0;
        static_cast<void>(aegir::bootstrap::window(&window_base, &window_bytes));
        aegir::debug_write("      my own address space's root: cap ");
        aegir::debug_write_unsigned(vspace_slot);
        aegir::debug_write(", with a window of my own from ");
        aegir::debug_write_hex(window_base);
        aegir::debug_write(", ");
        aegir::debug_write_unsigned(window_bytes / 1024 / 1024);
        aegir::debug_write(" MiB of it\n");
    }
    uint64_t binaries_address = 0;
    uint32_t binaries_bytes = 0;
    if (aegir::bootstrap::binaries(&binaries_address, &binaries_bytes)) {
        aegir::debug_write("      the initrd: ");
        aegir::debug_write_unsigned(binaries_bytes / 1024);
        aegir::debug_write(" KiB at ");
        aegir::debug_write_hex(binaries_address);
        aegir::debug_write(", to start processes from\n");
    }
    for (uint32_t d = 0;; ++d) {
        uint64_t grant_physical = 0;
        uint32_t grant_bytes = 0;
        uint64_t grant_slot = 0;
        if (!aegir::bootstrap::device_capability(d, &grant_physical, &grant_bytes,
                                                 &grant_slot)) {
            break;
        }
        aegir::debug_write("      a device to hand on: physical ");
        aegir::debug_write_hex(grant_physical);
        aegir::debug_write(", ");
        aegir::debug_write_unsigned(grant_bytes);
        aegir::debug_write(" bytes, frame at cap ");
        aegir::debug_write_unsigned(grant_slot);
        aegir::debug_write("\n");
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
