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
 * The map this service exists for -- bus -> device id -> the service process
 * that handles it (specs/services.md) -- is built from the machine's own
 * description of itself: the tree says which devices there are, the registry
 * says which driver handles which kind, the grants director delegated say
 * which of them are ours to drive, and the join of the three is what gets
 * spawned.
 */

#include <aegir/block.h>
#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/descriptor.h>
#include <aegir/devtree.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/vspace.h>
#include <aegir/ipc/port.h>
#include <aegir/nmspace.h>
#include <aegir/log.h>
#include <aegir/registry.h>
#include <aegir/spawn/initrd.h>
#include <aegir/spawn/process.h>
#include <aegir/virtio/input.h>
#include <aegir/virtio/mmio.h>
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

/** A driver the registry knows: which compatible string -- and, on a virtio
 *  transport, which probed device id -- it handles, what it starts from, and
 *  what its instances are called. The registry is *data*: it is parsed from a
 *  file the initrd carries (manifests/drivers.registry), so adding a driver is
 *  adding a row, not a code change (specs/services.md). The pointers are views
 *  into that text, which is mapped for this service's lifetime. */
struct DriverRow {
    char const *compatible;
    uint32_t compatible_length;
    uint32_t virtio_id; /* 0: the compatible alone decides; else the registers must say this */
    /* 0: any kind of the id's family. Else which member the device must be,
     * read from the config space's EV_BITS at the probe: 1/2/3 = key/rel/abs,
     * the EV_KEY/EV_REL/EV_ABS numbering (aegir/input.h). virtio id 18 is the
     * keyboard, the mouse AND the tablet -- the id is the family, the kind is
     * config. */
    uint8_t evtype;
    char const *name_prefix; /* instances are prefix.busN: "blk" + "virtio" -> blk.virtio0 */
    uint32_t name_prefix_length;
    char const *bus;
    uint32_t bus_length;
    char const *binary;
    uint32_t binary_length;
    uint32_t memory_bits; /* the virtqueue it lays out */
    uint32_t window_bits; /* the shared window its port serves through */
};

/** One cell of the map: a device the tree describes that a registry row claims,
 *  joined with the frame director granted for it. `frame` stays 0 -- reported,
 *  not driven -- when the join finds no grant or the probe finds a different
 *  device behind the transport. */
struct Binding {
    DriverRow const *row;
    uint64_t base;    /* where the tree puts the register window */
    uint32_t bytes;   /* the granted frame's size, set at the join */
    uint32_t irq;     /* 0: none */
    seL4_CPtr frame;  /* the granted capability; 0 until the join */
    char const *name; /* the instance name, built at the join */
    uint32_t name_length;
    bool spawned; /* a driver is running for it -- what the registry reports */
};

/* The convention a supervising server keeps -- a call carries the caller's
 * badge with the top bit set, a signal arrives bare -- lives in aegir-ipc
 * now that two services keep it (aegir/ipc/port.h's kCallMark, which says
 * why the badge and never the message length). */
constexpr seL4_Word kCallMark = aegir::ipc::kCallMark;

bool compatible_is(aegir::devtree::Device const &device, DriverRow const &row) noexcept
{
    if (device.compatible_length != row.compatible_length) {
        return false;
    }
    for (uint32_t i = 0; i < row.compatible_length; ++i) {
        if (device.compatible[i] != row.compatible[i]) {
            return false;
        }
    }
    return true;
}

/** Two rows name the same transport. */
bool same_compatible(DriverRow const &a, DriverRow const &b) noexcept
{
    if (a.compatible_length != b.compatible_length) {
        return false;
    }
    for (uint32_t i = 0; i < a.compatible_length; ++i) {
        if (a.compatible[i] != b.compatible[i]) {
            return false;
        }
    }
    return true;
}

/* A map string into a Row field: bounded, and NUL-terminated whatever the
 * source's length -- the registry's rows cross an IPC as words, and a reader
 * that trusts a missing NUL reads into the next field. */
void copy_out(char *out, uint32_t out_bytes, char const *in, uint32_t in_length) noexcept
{
    uint32_t at = 0;
    while (in != nullptr && at + 1 < out_bytes && at < in_length) {
        out[at] = in[at];
        ++at;
    }
    out[at] = '\0';
}

/** A bound port's window is the storage stack's to hand on when its instance
 *  says blk: the partition manager pairs the granted window frames with the
 *  block ports it picked, and it pairs them 4 KiB at a time -- a window it
 *  cannot serve (a scanout's mega pages) is never granted to it. */
bool block_window(char const *name, uint32_t length) noexcept
{
    return length >= 4 && name[0] == 'b' && name[1] == 'l' && name[2] == 'k' && name[3] == '.';
}

/** One binding as the registry answers it. */
void fill_row(Binding const &binding, aegir::registry::Row *row) noexcept
{
    copy_out(row->instance, sizeof(row->instance), binding.name, binding.name_length);
    copy_out(row->compatible, sizeof(row->compatible), binding.row->compatible,
             binding.row->compatible_length);
    copy_out(row->binary, sizeof(row->binary), binding.row->binary,
             binding.row->binary_length);
    row->base = binding.base;
    row->bytes = binding.bytes;
    row->irq = binding.irq;
    row->window_bits = binding.row->window_bits;
    row->bound = binding.spawned ? 1 : 0;
}

/** The tree walk that builds the candidate list. Two passes over the same walk
 *  -- count, then fill -- so the bindings array is exactly as large as the tree
 *  says it needs to be: a machine with more devices gets a bigger map, not a
 *  full one. */
class MapBuilder : public aegir::devtree::Tree::Visitor {
public:
    bool device(aegir::devtree::Device const &device) override {
        if (!device.has_region) {
            return true;
        }
        for (uint32_t r = 0; r < row_count; ++r) {
            if (!compatible_is(device, rows[r])) {
                continue;
            }
            if (fill != nullptr && count < capacity) {
                fill[count] = Binding{&rows[r], device.base, 0,
                                      device.has_interrupt ? device.interrupt : 0, 0,
                                      nullptr, 0, false};
            }
            ++count;
            break;
        }
        return true;
    }

    DriverRow const *rows = nullptr;
    uint32_t row_count = 0;
    Binding *fill = nullptr;
    uint32_t capacity = 0;
    uint32_t count = 0;
};

}  // namespace

namespace {
aegir::mem::Allocator g_objects(nullptr);
/* The window and the arena a spawner works through: static for the same reason
 *  the allocator is -- a spawned process has two pages of stack, and these
 *  tables do not fit on it (specs/userland.md). */
aegir::mem::Scratch g_scratch(nullptr);
aegir::mem::Account g_account{"devicemgr", 0, 0, 0};
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
            g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::kCNodeBits) - first_free, 0);
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
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    bool const given_vspace = aegir::bootstrap::capability("vspace", 6, &vspace_slot) &&
                              aegir::bootstrap::window(&window_base, &window_bytes);
    if (given_vspace) {
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

    /* Interrupt issue is the third thing director delegates: IRQControl is
     * the kernel's one well of handler caps, and minting them belongs to
     * whoever binds drivers to devices. Its absence is reported, not fatal --
     * a driver without a handler polls. */
    uint64_t irqcontrol_slot = 0;
    if (aegir::bootstrap::capability("irqcontrol", 10, &irqcontrol_slot)) {
        write_line("irqcontrol", "given");
    } else {
        write_line("irqcontrol", "not given -- drivers will poll");
    }

    /* Spawning: what this service was given the authority for (specs/services.md).
     * The map is the join of three sources: the tree says which devices the
     * machine has, the registry says which driver handles which kind, and the
     * grants director delegated say which of them are ours to drive. */
    bool const can_spawn = given_vspace && untyped_slot != 0 && pool_slot != 0 &&
                           binaries_address != 0 && binaries_bytes != 0;
    if (!can_spawn) {
        write_line("spawning", "not everything it takes was given");
    } else if (!g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                                static_cast<uintptr_t>(window_base),
                                static_cast<uintptr_t>(window_base + window_bytes),
                                &g_objects)) {
        write_line("FAIL", "the window I was given could not be adopted");
    } else {
        aegir::mem::Arena arena(g_objects, g_scratch, g_account);
        aegir::spawn::Initrd const initrd(reinterpret_cast<void const *>(binaries_address),
                                          binaries_bytes);
        if (!initrd.valid()) {
            write_line("FAIL", "the initrd copy I was given cannot be read");
        } else {
            /* Our own CNode, at its own depth: a service's own-CNode cap is a raw
             * copy with guard 0 and radix kCNodeBits, so the spawner must address
             * mint sources through it -- seL4_CapInitThreadCNode is the *root
             * task's* name for its CSpace, and this is not the root task
             * (aegir/bootstrap.h). */
            aegir::spawn::Spawner spawner(g_objects, g_scratch, arena, initrd,
                                          static_cast<seL4_CPtr>(pool_slot),
                                          static_cast<seL4_CPtr>(aegir::bootstrap::kSlotOwnCNode),
                                          aegir::bootstrap::kCNodeBits);

            /* The registry, parsed from the file the initrd carries: the map is
             * the join of the tree with *this* table, and the table is data, so
             * adding a driver is adding a row to a file (specs/services.md). Two
             * passes over the text -- count, then fill -- so the rows array is
             * exactly the file's size. The archive's name for a file is its
             * basename (apps/aegir-director/CMakeLists.txt), so the registry is
             * "drivers.registry" here. */
            uint64_t registry_bytes = 0;
            void const *registry_text = initrd.find("drivers.registry", 16, &registry_bytes);
            DriverRow *rows = nullptr;
            uint32_t row_count = 0;
            if (registry_text == nullptr) {
                write_line("FAIL", "the initrd carries no driver registry");
            } else {
                aegir::descriptor::Reader counting(registry_text, registry_bytes);
                while (counting.next_row()) {
                    ++row_count;
                }
                rows = static_cast<DriverRow *>(
                    arena.allocate(sizeof(DriverRow) * (row_count != 0 ? row_count : 1)));
                if (rows == nullptr) {
                    write_line("FAIL", "no room for the driver registry");
                    row_count = 0;
                } else {
                    uint32_t parsed = 0;
                    aegir::descriptor::Reader reader(registry_text, registry_bytes);
                    while (parsed < row_count && reader.next_row()) {
                        DriverRow row{};
                        bool fields_ok = true;
                        aegir::descriptor::Field field;
                        while (reader.next_field(field)) {
                            if (aegir::descriptor::key_is(field, "compatible")) {
                                row.compatible = field.value;
                                row.compatible_length = field.value_length;
                            } else if (aegir::descriptor::key_is(field, "id")) {
                                row.virtio_id = static_cast<uint32_t>(
                                    aegir::descriptor::number(field, &fields_ok));
                            } else if (aegir::descriptor::key_is(field, "evtype")) {
                                /* The words are the file's; the numbers are
                                 * EV_KEY/EV_REL/EV_ABS (aegir/input.h). */
                                if (aegir::descriptor::value_is(field, "key")) {
                                    row.evtype = 1;
                                } else if (aegir::descriptor::value_is(field, "rel")) {
                                    row.evtype = 2;
                                } else if (aegir::descriptor::value_is(field, "abs")) {
                                    row.evtype = 3;
                                } else {
                                    fields_ok = false;
                                }
                            } else if (aegir::descriptor::key_is(field, "prefix")) {
                                row.name_prefix = field.value;
                                row.name_prefix_length = field.value_length;
                            } else if (aegir::descriptor::key_is(field, "bus")) {
                                row.bus = field.value;
                                row.bus_length = field.value_length;
                            } else if (aegir::descriptor::key_is(field, "binary")) {
                                row.binary = field.value;
                                row.binary_length = field.value_length;
                            } else if (aegir::descriptor::key_is(field, "memory")) {
                                row.memory_bits = static_cast<uint32_t>(
                                    aegir::descriptor::number(field, &fields_ok));
                            } else if (aegir::descriptor::key_is(field, "window")) {
                                row.window_bits = static_cast<uint32_t>(
                                    aegir::descriptor::number(field, &fields_ok));
                            }
                        }
                        /* A window of zero bits is a complete row, not a broken
                         * one: a port whose answers ride in the envelope needs
                         * no shared window (aegir/entropy.h). */
                        if (!fields_ok || row.compatible == nullptr || row.binary == nullptr ||
                            row.name_prefix == nullptr || row.bus == nullptr ||
                            row.memory_bits == 0) {
                            write_line("FAIL", "a row of the driver registry is not complete");
                            continue;
                        }
                        rows[parsed++] = row;
                    }
                    row_count = parsed;
                    aegir::debug_write("      registry: ");
                    aegir::debug_write_unsigned(row_count);
                    aegir::debug_write(row_count == 1 ? " driver, from the file\n"
                                                      : " drivers, from the file\n");
                }
            }

            /* The candidate cells: every tree device a registry row claims.
             * Count, then fill, so the array is exactly the tree's size. */
            MapBuilder counter;
            counter.rows = rows;
            counter.row_count = row_count;
            static_cast<void>(tree.walk(counter));
            Binding *bindings = nullptr;
            uint32_t binding_count = 0;
            if (counter.count > 0) {
                bindings = static_cast<Binding *>(
                    arena.allocate(sizeof(Binding) * counter.count));
                if (bindings == nullptr) {
                    write_line("FAIL", "no room for the device map");
                } else {
                    MapBuilder filler;
                    filler.rows = rows;
                    filler.row_count = row_count;
                    filler.fill = bindings;
                    filler.capacity = counter.count;
                    static_cast<void>(tree.walk(filler));
                    binding_count = filler.count;
                }
            }

            /* The join and the probe, binding by binding: a candidate becomes
             * real when director granted its frame, and -- on a virtio transport,
             * which does not say what is behind it -- when the registers say the
             * device id the row answers to. */
            for (uint32_t b = 0; b < binding_count; ++b) {
                Binding &binding = bindings[b];
                for (uint32_t d = 0;; ++d) {
                    uint64_t grant_physical = 0;
                    uint32_t grant_bytes = 0;
                    uint64_t grant_slot = 0;
                    if (!aegir::bootstrap::device_capability(d, &grant_physical,
                                                             &grant_bytes, &grant_slot)) {
                        break;
                    }
                    if (grant_physical == binding.base) {
                        binding.frame = static_cast<seL4_CPtr>(grant_slot);
                        binding.bytes = grant_bytes;
                        break;
                    }
                }
                if (binding.frame == 0) {
                    aegir::debug_write("      map: ");
                    aegir::debug_write(binding.row->compatible, binding.row->compatible_length);
                    aegir::debug_write(" at ");
                    aegir::debug_write_hex(binding.base);
                    aegir::debug_write(": the tree describes it, but no frame was granted for it\n");
                    continue;
                }
                if (binding.row->virtio_id != 0) {
                    volatile uint8_t *mapped =
                        static_cast<volatile uint8_t *>(g_scratch.map(binding.frame));
                    if (mapped == nullptr) {
                        write_line("FAIL", "a device frame could not be mapped for probing");
                        binding.frame = 0;
                        continue;
                    }
                    auto const *registers =
                        reinterpret_cast<volatile uint32_t const *>(mapped);
                    uint32_t const probed_magic = registers[aegir::virtio::kMagicValue / 4];
                    uint32_t const probed_id = registers[aegir::virtio::kDeviceId / 4];
                    /* The kind within the family, when a row asks for it: id
                     * 18 is the keyboard, the mouse AND the tablet, and a
                     * row's `evtype` key is its claim on one member -- the
                     * answer is the config space's EV_BITS
                     * (aegir/virtio/input.h), read while the frame is mapped.
                     * Those selectors are the input family's own registers,
                     * so the read happens only when the id is 18 and a row
                     * carries evtype -- written to a block device's config
                     * page they would be its capacity. */
                    uint8_t probed_class = 0;
                    if (probed_id == aegir::virtio::kDeviceIdInput) {
                        bool asked = false;
                        for (uint32_t r = 0; r < row_count && !asked; ++r) {
                            asked = rows[r].virtio_id == probed_id && rows[r].evtype != 0 &&
                                    same_compatible(rows[r], *binding.row);
                        }
                        if (asked) {
                            namespace inputcfg = aegir::virtio::input;
                            volatile uint8_t *config = mapped + aegir::virtio::kConfig;
                            auto raises = [&](uint8_t type) noexcept {
                                config[inputcfg::kRegSelect] = inputcfg::kSelectEvBits;
                                config[inputcfg::kRegSubsel] = type;
                                return config[inputcfg::kRegSize] != 0;
                            };
                            /* ABS settles it (the tablet), then REL (the
                             * mouse); what is left is the keyboard -- the
                             * driver's own announcement reads the same bits,
                             * and the two must agree. */
                            probed_class = raises(3) ? 3 : (raises(2) ? 2 : 1);
                        }
                    }
                    g_scratch.unmap(binding.frame);
                    if (probed_magic != aegir::virtio::kMagic) {
                        binding.frame = 0;
                        continue;
                    }
                    /* The row for this device: the id names the family, and
                     * when the family's rows carry evtype the member must
                     * match it -- an exact kind wins, a row without evtype
                     * claims any kind. (The candidate match was on the
                     * compatible string, which every transport on the bus
                     * shares -- the first row that claims it is not
                     * necessarily the row for the device behind this one, and
                     * the probe is what says which is.) */
                    DriverRow const *match = nullptr;
                    for (uint32_t r = 0; r < row_count; ++r) {
                        if (rows[r].virtio_id != probed_id ||
                            !same_compatible(rows[r], *binding.row)) {
                            continue;
                        }
                        if (rows[r].evtype == probed_class) {
                            match = &rows[r];
                            break;
                        }
                        if (rows[r].evtype == 0 && match == nullptr) {
                            match = &rows[r];
                        }
                    }
                    if (match == nullptr) {
                        aegir::debug_write("      map: virtio device ");
                        aegir::debug_write_unsigned(probed_id);
                        aegir::debug_write(" at ");
                        aegir::debug_write_hex(binding.base);
                        aegir::debug_write(": no driver in the registry\n");
                        binding.frame = 0;
                        continue;
                    }
                    binding.row = match;
                }
                /* The instance name: which kind, which bus, and which one it is --
                 * blk.virtio0 rather than blkdriver, because the second device of a
                 * kind is not the first (specs/services.md). Instances count only
                 * bindings that made it this far. */
                uint32_t instance = 0;
                for (uint32_t o = 0; o < b; ++o) {
                    if (bindings[o].name != nullptr && bindings[o].row == binding.row) {
                        ++instance;
                    }
                }
                uint32_t digits = 1;
                for (uint32_t t = instance; t >= 10; t /= 10) {
                    ++digits;
                }
                DriverRow const *row = binding.row;
                uint32_t const name_bytes =
                    row->name_prefix_length + 1 + row->bus_length + digits;
                auto *named = static_cast<char *>(arena.allocate(name_bytes));
                if (named == nullptr) {
                    write_line("FAIL", "no room to name a device instance");
                    binding.frame = 0;
                    continue;
                }
                uint32_t at = 0;
                for (uint32_t c = 0; c < row->name_prefix_length; ++c) {
                    named[at++] = row->name_prefix[c];
                }
                named[at++] = '.';
                for (uint32_t c = 0; c < row->bus_length; ++c) {
                    named[at++] = row->bus[c];
                }
                for (uint32_t c = digits; c > 0; --c) {
                    uint32_t divisor = 1;
                    for (uint32_t m = 1; m < c; ++m) {
                        divisor *= 10;
                    }
                    named[at++] = static_cast<char>('0' + (instance / divisor) % 10);
                }
                binding.name = named;
                binding.name_length = name_bytes;

                aegir::debug_write("      map: ");
                aegir::debug_write(named, name_bytes);
                aegir::debug_write(": ");
                aegir::debug_write(row->compatible, row->compatible_length);
                aegir::debug_write(" at ");
                aegir::debug_write_hex(binding.base);
                if (binding.irq != 0) {
                    aegir::debug_write(" irq ");
                    aegir::debug_write_unsigned(binding.irq);
                }
                aegir::debug_write(" -> ");
                aegir::debug_write(row->binary, row->binary_length);
                aegir::debug_write(", frame cap ");
                aegir::debug_write_unsigned(binding.frame);
                aegir::debug_write("\n");
            }

            /* The cap the children call is minted from the *delegatable* copy:
             * our own log.main is already badged with who we are, and a badged
             * endpoint cap cannot be minted again -- so director grants the
             * ports a spawning service's children need under a "spawn:" name,
             * unbadged, for exactly this (specs/services.md). */
            uint64_t log_slot = 0;
            bool const have_log =
                aegir::bootstrap::capability("spawn:log.main", 14, &log_slot);
            /* The namespace's caller half, for the partition manager's part
             * of registering the volumes it starts (specs/vfs.md): it has
             * Grant, so the volume port's caller half can ride the
             * registration call. */
            uint64_t nmspace_slot = 0;
            bool const have_nmspace =
                aegir::bootstrap::capability("spawn:vfs.namespace", 19, &nmspace_slot);
            if (binding_count > 0 && !have_log) {
                write_line("FAIL", "no delegatable log.main was given");
            }
            /* Badges for the processes we start count from 256: the low badges are
             * director's boot set, and until the badge space is a designed thing,
             * a spawning service's children live in a range of their own
             * (specs/services.md). */
            /* What bound, collected for the services that consume the map: the
             * caller half of each bound driver's port and the pristine
             * window-cap set its clients map -- empty for a driver whose row
             * declares no window. The partition manager picks the block ports
             * out of the list by name; the registry's `open` is where the
             * others are reached (specs/services.md). */
            struct BoundPort {
                seL4_CPtr port;
                seL4_CPtr window;   /* the pristine set: one frame cap per page */
                seL4_CPtr children; /* a second pristine set, for the port's
                                       clients to hand to *their* children:
                                       nobody ever maps it, so mints from it
                                       stay mappable (kernel/src/arch/riscv/
                                       kernel/vspace.c:869-878) */
                uint32_t window_pages;
                uint32_t window_page_bits; /* what the frames of the window
                                              set are: 4 KiB, or mega pages
                                              from 2 MiB windows up */
                uint64_t window_physical;
                char const *name;
                uint32_t name_length;
                uint32_t binding; /* its row in the map: what `open` indexes by */
            };
            auto *bound = static_cast<BoundPort *>(arena.allocate(
                sizeof(BoundPort) * (binding_count != 0 ? binding_count : 1)));
            uint32_t bound_count = 0;
            /* The port the map is asked through is the manifest's:
             * `owns = devmgr.registry` is what lets a service director
             * starts hold a caller half, which an endpoint made here could
             * never reach. The owner half carries every right, because the
             * mints an `open` answers with keep only what the source holds
             * (specs/services.md). The caller half goes to the services the
             * map is for -- the partition manager first. */
            uint64_t registry_slot = 0;
            seL4_CPtr const registry_endpoint =
                aegir::bootstrap::capability(aegir::registry::kPortName,
                                             aegir::registry::kPortNameLength,
                                             &registry_slot)
                    ? static_cast<seL4_CPtr>(registry_slot)
                    : 0;
            if (registry_endpoint == 0) {
                write_line("FAIL", "the registry's port was not given");
            }
            /* Set when the partition manager is up: its ready arrives on the
             * supervision notification the spawn made for it -- our half of
             * it, which we keep here, is the receiving half the serve loop
             * binds. The badge is named here for the same reason: the serve
             * loop matches a signal against it. */
            bool partmgr_running = false;
            seL4_CPtr partmgr_supervision = 0;
            uint64_t const partmgr_badge = 256u + binding_count;
            for (uint32_t b = 0; have_log && b < binding_count; ++b) {
                Binding &binding = bindings[b];
                if (binding.frame == 0) {
                    continue;
                }
                DriverRow const *driver = binding.row;
                /* The queue's memory: carved from our untyped, paged, and handed to
                 * the child as frames to map -- the same shape director gives a
                 * service that declares memory (specs/authority.md). */
                aegir::mem::Account child_account{binding.name, 0, 0, 0};
                seL4_Error queue_error = seL4_NoError;
                uint64_t queue_physical = 0;
                seL4_CPtr const queue = g_objects.carve_untyped(driver->memory_bits,
                                                                child_account, &queue_error,
                                                                &queue_physical);
                if (queue == 0) {
                    write_line("FAIL", "no memory for a driver's virtqueue");
                    continue;
                }
                seL4_CPtr memory_frame = 0;
                uint32_t const pages = (1u << driver->memory_bits) / 4096u;
                bool paged = true;
                for (uint32_t p = 0; p < pages; ++p) {
                    seL4_Error page_error = seL4_NoError;
                    seL4_CPtr const frame = g_objects.carve_page(queue, child_account,
                                                                 &page_error);
                    if (frame == 0) {
                        paged = false;
                        break;
                    }
                    if (p == 0) {
                        memory_frame = frame;
                    }
                }
                if (!paged) {
                    write_line("FAIL", "a driver's virtqueue could not be turned into pages");
                    continue;
                }

                /* The shared window the driver's port serves through, when the
                 * row asks for one: carved the same way as the queue, mapped
                 * into the child beside its memory, and mapped into each client
                 * the port is later delegated to -- what a request moves is in
                 * the window, not the message (aegir/block.h,
                 * specs/services.md). A row with zero window bits gets none of
                 * this: the port's answers ride in the envelope itself
                 * (aegir/entropy.h). */
                seL4_CPtr window_frame = 0;
                uint64_t window_physical = 0;
                uint32_t window_pages = 0;
                uint32_t window_page_bits = seL4_PageBits;
                seL4_CPtr window_client = 0;
                seL4_CPtr window_children = 0;
                seL4_CPtr window_smoke = 0;
                if (driver->window_bits != 0) {
                /* A window of 2 MiB or more rides as mega pages: a framebuffer
                 * window of thousands of 4 KiB frames is thousands of caps, and
                 * every CSpace here holds 1024 slots (kCNodeBits) -- the per-page
                 * path tops out long before a screen does. 4 KiB stays the shape
                 * for windows a port's clients copy through (blk's 64 KiB). */
                window_page_bits =
                    driver->window_bits >= seL4_LargePageBits ? seL4_LargePageBits
                                                              : seL4_PageBits;
                seL4_Error window_error = seL4_NoError;
                seL4_CPtr const window_untyped =
                    g_objects.carve_untyped(driver->window_bits, child_account,
                                            &window_error, &window_physical);
                if (window_untyped == 0) {
                    write_line("FAIL", "no memory for a driver's shared window");
                    continue;
                }
                window_pages = (1u << driver->window_bits) >> window_page_bits;
                bool window_paged = true;
                for (uint32_t p = 0; p < window_pages; ++p) {
                    seL4_Error page_error = seL4_NoError;
                    seL4_CPtr const frame = g_objects.carve_page(window_untyped, child_account,
                                                                 &page_error, window_page_bits);
                    if (frame == 0) {
                        window_paged = false;
                        break;
                    }
                    if (p == 0) {
                        window_frame = frame;
                    }
                }
                if (!window_paged) {
                    write_line("FAIL", "a driver's shared window could not be turned into pages");
                    continue;
                }
                /* A frame's first mapping pins it to that address space: the
                 * mapped ASID lives in the *capability*
                 * (kernel/src/arch/riscv/kernel/vspace.c:869-878), so one cap
                 * can never serve two VSpaces -- while a copy made before any
                 * mapping carries no ASID yet and may be mapped into another.
                 * The window's consumers each need their own set of caps, made
                 * now, before anything maps: the originals go to the child,
                 * one copy set stays pristine as the source for every client
                 * grant (the partition manager's frames are minted from it
                 * straight into the child's CSpace), and one set is ours to
                 * peek through for the smoke. A copy made after a mapping
                 * would inherit that mapping's ASID and be useless. */
                auto mint_window_set = [&window_frame, window_pages]() -> seL4_CPtr {
                    seL4_CPtr base = 0;
                    for (uint32_t p = 0; p < window_pages; ++p) {
                        seL4_CPtr const slot = g_objects.alloc_slot();
                        if (slot == 0 ||
                            seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, slot,
                                            aegir::bootstrap::kCNodeBits,
                                            aegir::bootstrap::kSlotOwnCNode, window_frame + p,
                                            aegir::bootstrap::kCNodeBits, seL4_AllRights,
                                            0) != seL4_NoError) {
                            return 0;
                        }
                        if (p == 0) {
                            base = slot;
                        }
                    }
                    return base;
                };
                window_client = mint_window_set();
                window_children = mint_window_set();
                window_smoke = mint_window_set();
                if (window_client == 0 || window_children == 0 || window_smoke == 0) {
                    write_line("FAIL", "the shared window's frames could not be copied");
                    continue;
                }
                }
                /* The port the driver serves: one endpoint per device, made
                 * here because the endpoint is the spawner's to make -- the
                 * child gets the owner half, we keep the caller half. */
                seL4_Error port_error = seL4_NoError;
                seL4_CPtr const block_port =
                    g_objects.alloc_object(seL4_EndpointObject, seL4_EndpointBits,
                                           child_account, &port_error);
                if (block_port == 0) {
                    write_line("FAIL", "no memory for a driver's port");
                    continue;
                }
                /* The driver's interrupt, when the tree says the device raises
                 * one: one handler cap per IRQ is all the kernel issues (a
                 * second Get is seL4_RevokeFirst), so it is made here, at the
                 * binding, paired with a notification before the child exists,
                 * and armed -- the first Ack is what lets signals in
                 * (projects/sel4test/apps/sel4test-driver/src/main.c:555-582).
                 * The child gets the pair: it waits on one and acks on the
                 * other. */
                seL4_CPtr irq_handler = 0;
                seL4_CPtr irq_notification = 0;
                if (binding.irq != 0 && irqcontrol_slot != 0) {
                    irq_handler = g_objects.alloc_slot();
                    seL4_Error const get_error =
                        irq_handler == 0
                            ? seL4_NotEnoughMemory
                            : seL4_IRQControl_Get(
                                  static_cast<seL4_IRQControl>(irqcontrol_slot),
                                  binding.irq, aegir::bootstrap::kSlotOwnCNode,
                                  irq_handler, aegir::bootstrap::kCNodeBits);
                    seL4_Error notify_error = seL4_NoError;
                    irq_notification =
                        g_objects.alloc_object(seL4_NotificationObject,
                                               seL4_NotificationBits, child_account,
                                               &notify_error);
                    if (get_error != seL4_NoError || irq_notification == 0 ||
                        seL4_IRQHandler_SetNotification(irq_handler,
                                                        irq_notification) !=
                            seL4_NoError ||
                        seL4_IRQHandler_Ack(irq_handler) != seL4_NoError) {
                        write_line("FAIL", "a driver's interrupt could not be issued");
                        continue;
                    }
                }

                aegir::spawn::PortGrant const ports[] = {
                    {aegir::log::kPortName, aegir::log::kPortNameLength,
                     aegir::bootstrap::kSlotFirstDeclared,
                     static_cast<seL4_CPtr>(log_slot), seL4_CapRights_new(1, 0, 0, 1),
                     256u + b, 0},
                    /* The driver owns this port: it receives, so Read is the
                     * whole grant -- the caller half never leaves us
                     * (specs/services.md). */
                    {"port", 4, aegir::bootstrap::kSlotFirstDeclared + 1, block_port,
                     seL4_CapRights_new(0, 0, 1, 0), 0, 0},
                    /* The interrupt pair: the driver waits on the notification
                     * (Read is the whole grant) and acks on the handler after
                     * each signal. Only present when the binding has an IRQ --
                     * a driver that finds neither polls. */
                    {"irq.notify", 10, aegir::bootstrap::kSlotFirstDeclared + 2,
                     irq_notification, seL4_CapRights_new(0, 0, 1, 0), 0, 0},
                    {"irq.handler", 11, aegir::bootstrap::kSlotFirstDeclared + 3,
                     irq_handler, seL4_AllRights, 0, 0},
                };
                aegir::spawn::DeviceGrant const devices[] = {
                    {binding.base, binding.bytes, binding.frame},
                };
                aegir::spawn::Request request{};
                request.name = binding.name;
                request.name_length = binding.name_length;
                request.binary = driver->binary;
                request.binary_length = driver->binary_length;
                request.account = "system";
                request.account_length = 6;
                /* The priority the manifest would have said: the one just below
                 * ours, which is also the MCP we were given -- so conferring it
                 * is within what we hold. */
                request.priority = seL4_MaxPrio - 1;
                request.ports = ports;
                request.port_count = irq_handler != 0 ? 4 : 2;
                request.device_frame = binding.frame;
                request.device_bytes = binding.bytes;
                request.device_physical = binding.base;
                request.device_grants = devices;
                request.device_grant_count = 1;
                request.memory_frame = memory_frame;
                request.memory_bytes = 1u << driver->memory_bits;
                request.window_frame = window_frame;
                request.window_bytes =
                    driver->window_bits != 0 ? (1u << driver->window_bits) : 0;
                request.window_physical = window_physical;
                request.window_page_bits = window_page_bits;
                /* The queue's descriptors carry physical addresses the device
                 * reads, and a capability does not say where it is -- so the
                 * physical base travels beside the frames, the way director's
                 * carve does (specs/authority.md). */
                request.untyped_physical = queue_physical;
                request.untyped_bits = driver->memory_bits;
                /* Its faults come to whoever it was spawned from -- that is what
                 * a supervisor *is*. The endpoint is one we make, not the one we
                 * were given: ours is badged with who *we* are, and a badged
                 * endpoint cap cannot be minted again for the child's badge (the
                 * kernel's own words: "Mutated cap would be invalid"). */
                seL4_Error fault_error = seL4_NoError;
                seL4_CPtr const fault_endpoint =
                    g_objects.alloc_object(seL4_EndpointObject, seL4_EndpointBits,
                                           child_account, &fault_error);
                if (fault_endpoint == 0) {
                    write_line("FAIL", "no memory for a child's fault endpoint");
                    continue;
                }
                request.fault_endpoint = fault_endpoint;
                request.badge = 256u + b;

                aegir::spawn::Process process{};
                if (!spawner.spawn(request, child_account, process)) {
                    aegir::debug_write("      FAIL spawning ");
                    aegir::debug_write(binding.name, binding.name_length);
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
                    continue;
                }
                aegir::debug_write("      spawned ");
                aegir::debug_write(binding.name, binding.name_length);
                aegir::debug_write(" for ");
                aegir::debug_write(driver->compatible, driver->compatible_length);
                aegir::debug_write(" at ");
                aegir::debug_write_hex(binding.base);
                aegir::debug_write(", badge ");
                aegir::debug_write_unsigned(request.badge);
                aegir::debug_write("\n");
                binding.spawned = true;
                /* Ready is a signal on the supervision notification, badged with who
                 * it is -- the same protocol director's boot uses, because a spawned
                 * process does not know who spawned it (specs/director.md). */
                seL4_Word ready_badge = 0;
                seL4_Wait(process.supervision, &ready_badge);

                /* Smoke: use the port the way a client will. The smoke speaks
                 * the block protocol -- Identify fills the window with who the
                 * device says it is ("BD0" is the driver's own name for itself,
                 * not something we assigned; aegir/block.h, specs/services.md),
                 * and a read of sector 0 lands in the same window without a
                 * byte crossing the message -- so it is for the drivers whose
                 * port is one. A driver with no window (the entropy source
                 * answers in the envelope itself) is bound without it; the
                 * registry's `open` is where its port gets used. We hold the
                 * caller half because we made the endpoint; the window's
                 * frames stay ours as well, and are what a later client
                 * maps. */
                bool const is_block = driver->name_prefix_length == 3 &&
                                      driver->name_prefix[0] == 'b' &&
                                      driver->name_prefix[1] == 'l' &&
                                      driver->name_prefix[2] == 'k';
                if (is_block) {
                auto *shared = static_cast<uint8_t *>(g_scratch.map(window_smoke));
                if (shared == nullptr) {
                    uint64_t const window_map_error = g_scratch.last_error();
                    aegir::debug_write("      FAIL the shared window (slot ");
                    aegir::debug_write_unsigned(window_smoke);
                    aegir::debug_write(") could not be mapped for the smoke read (seL4 error ");
                    aegir::debug_write_unsigned(window_map_error);
                    aegir::debug_write(")\n");
                    continue;
                }
                aegir::ipc::Consumer const block_caller(block_port);
                aegir::ipc::Reply const identity =
                    block_caller.call(aegir::block::kMethodIdentify, 0);
                if (identity.error != 0 || identity.word != sizeof(aegir::block::Identify)) {
                    write_line("FAIL", "the block port would not identify");
                    g_scratch.unmap(window_smoke);
                    continue;
                }
                auto const *who = reinterpret_cast<aegir::block::Identify const *>(shared);
                uint32_t name_length = 0;
                while (name_length < sizeof(who->name) && who->name[name_length] != '\0') {
                    ++name_length;
                }
                /* The window belongs to the call, not to us: the read below
                 * overwrites the identify answer, so the name is kept out of
                 * it. */
                char device_name[sizeof(who->name)];
                for (uint32_t i = 0; i < name_length; ++i) {
                    device_name[i] = who->name[i];
                }
                aegir::debug_write("      ");
                aegir::debug_write(device_name, name_length);
                aegir::debug_write(": ");
                aegir::debug_write_unsigned(who->sector_count);
                aegir::debug_write(" sectors of ");
                aegir::debug_write_unsigned(who->sector_size);
                aegir::debug_write(" bytes, window holds ");
                aegir::debug_write_unsigned(who->window_sectors);
                aegir::debug_write("\n");
                aegir::ipc::Reply const smoke =
                    block_caller.call(aegir::block::kMethodRead, aegir::block::pack_read(0, 1));
                if (smoke.error != 0 || smoke.word != 1) {
                    write_line("FAIL", "the block port would not read sector 0");
                } else {
                    aegir::debug_write("      ");
                    aegir::debug_write(device_name, name_length);
                    aegir::debug_write(" read sector 0 through the window, first 16: ");
                    for (unsigned i = 0; i < 16; ++i) {
                        aegir::debug_write_hex(shared[i]);
                        aegir::debug_write(" ");
                    }
                    aegir::debug_write("\n");
                }
                g_scratch.unmap(window_smoke);
                }
                /* The binding is whole: port served, window checked when the
                 * driver has one. What the partition manager gets is this
                 * list. */
                bound[bound_count] = BoundPort{block_port, window_client, window_children,
                                               window_pages, window_page_bits,
                                               window_physical, binding.name,
                                               binding.name_length, b};
                ++bound_count;
            }

            /* The partition manager, started once the drivers answer: it gets
             * the caller half of every bound block port, the pristine window
             * frames its reads move data through, and the authority to map
             * them -- an untyped, the ASID pool and its VSpace root
             * (specs/services.md). It is ours to start: the director knows
             * services, not the storage stack's insides. No initrd copy yet:
             * the initrd is 1.2 MiB and a copy per spawning service does not
             * fit a 1 MiB delegation, so what the filesystem launch travels
             * with is a narrower answer than "everything". */
            if (bound_count > 0) {
                static char const kPartmgrName[] = "partmgr";
                static char const kPartmgrBinary[] = "aegir-partmgr";
/* Room for its own objects and for what it carves for the
                 * filesystem services it starts: each one costs its image copy,
                 * its objects, and a 64 KiB window set of its own. 1 MiB held
                 * two partitions before the buddy allocator, whose node pool
                 * also comes out of this delegation (specs/allocator.md); three
                 * partitions and the nodes no longer fit, and the manager's own
                 * 68 MiB delegation is where the room is. Derived sizing -- from
                 * the children it will start -- is the follow-up. */
                constexpr uint32_t kPartmgrUntypedBits = 23;
                aegir::mem::Account partmgr_account{"partmgr", 0, 0, 0};
                seL4_Error untyped_error = seL4_NoError;
                uint64_t partmgr_physical = 0;
                seL4_CPtr const partmgr_untyped =
                    g_objects.carve_untyped(kPartmgrUntypedBits, partmgr_account,
                                            &untyped_error, &partmgr_physical);
                seL4_Error fault_error = seL4_NoError;
                seL4_CPtr const partmgr_fault =
                    g_objects.alloc_object(seL4_EndpointObject, seL4_EndpointBits,
                                           partmgr_account, &fault_error);
                uint32_t window_grant_count = 0;
                for (uint32_t i = 0; i < bound_count; ++i) {
                    /* Two groups per port: the partition manager's own pages,
                     * then the set reserved for the children it will start.
                     * Only the block ports' windows ride: they are the ports
                     * the manager pairs the frames with, 4 KiB at a time. */
                    if (block_window(bound[i].name, bound[i].name_length)) {
                        window_grant_count += 2 * bound[i].window_pages;
                    }
                }
                /* The registry's caller half rides with the manager's grants
                 * when the endpoint exists -- the map is for asking, and the
                 * partition manager is the first service that asks. */
                uint32_t const registry_rows = registry_endpoint != 0 ? 1 : 0;
                uint32_t const nmspace_rows = have_nmspace ? 1 : 0;
                auto *ports = static_cast<aegir::spawn::PortGrant *>(
                    arena.allocate(sizeof(aegir::spawn::PortGrant) *
                                   (4 + registry_rows + nmspace_rows + bound_count)));
                auto *frames = static_cast<aegir::spawn::DeviceGrant *>(arena.allocate(
                    sizeof(aegir::spawn::DeviceGrant) *
                    (window_grant_count != 0 ? window_grant_count : 1)));
                if (partmgr_untyped == 0 || partmgr_fault == 0 || ports == nullptr ||
                    frames == nullptr) {
                    write_line("FAIL", "no memory for the partition manager");
                } else {
                    ports[0] = {aegir::log::kPortName, aegir::log::kPortNameLength,
                                aegir::bootstrap::kSlotFirstDeclared,
                                static_cast<seL4_CPtr>(log_slot),
                                seL4_CapRights_new(1, 0, 0, 1), partmgr_badge, 0};
                    static char const kUntypedGrant[] = "untyped";
                    ports[1] = {kUntypedGrant, sizeof(kUntypedGrant) - 1,
                                aegir::bootstrap::kSlotFirstDeclared + 1, partmgr_untyped,
                                seL4_AllRights, 0, kPartmgrUntypedBits};
                    static char const kPoolGrant[] = "asid-pool";
                    ports[2] = {kPoolGrant, sizeof(kPoolGrant) - 1,
                                aegir::bootstrap::kSlotFirstDeclared + 2,
                                static_cast<seL4_CPtr>(pool_slot), seL4_AllRights, 0, 0};
                    /* The delegatable log, for the filesystem services it
                     * starts: its own log.main is badged with who it is, and a
                     * badged endpoint cap cannot be minted again -- so the
                     * unbadged copy travels down the same way it arrived
                     * (specs/services.md). */
                    static char const kSpawnLogGrant[] = "spawn:log.main";
                    ports[3] = {kSpawnLogGrant, sizeof(kSpawnLogGrant) - 1,
                                aegir::bootstrap::kSlotFirstDeclared + 3,
                                static_cast<seL4_CPtr>(log_slot), seL4_AllRights, 0, 0};
                    if (registry_rows != 0) {
                        /* The map, askable: the caller half, badged with who
                         * the manager is -- with the top bit set, the mark
                         * that tells a call apart from a signal when both
                         * wake the same receive (the serve loop keeps the
                         * convention). */
                        ports[4] = {aegir::registry::kPortName,
                                    aegir::registry::kPortNameLength,
                                    aegir::bootstrap::kSlotFirstDeclared + 4,
                                    registry_endpoint, seL4_CapRights_new(1, 0, 0, 1),
                                    partmgr_badge | kCallMark, 0};
                    }
                    if (nmspace_rows != 0) {
                        /* The namespace, badged with who the manager is and
                         * no call mark -- the VFS's port wakes its own
                         * receive only. */
                        ports[4 + registry_rows] = {
                            aegir::nmspace::kPortName, aegir::nmspace::kPortNameLength,
                            aegir::bootstrap::kSlotFirstDeclared + 4 + registry_rows,
                            static_cast<seL4_CPtr>(nmspace_slot),
                            seL4_CapRights_new(1, 1, 0, 1), partmgr_badge, 0};
                    }
                    /* Each block port arrives under the driver's instance name:
                     * the caller half, which is Write and GrantReply -- the
                     * kernel's own requirement of a capability that may be
                     * called (out/aegir/libsel4/include/interfaces/
                     * sel4_client.h:1202). The owner half stays here. */
                    for (uint32_t i = 0; i < bound_count; ++i) {
                        ports[4 + registry_rows + nmspace_rows + i] = {
                            bound[i].name, bound[i].name_length,
                            aegir::bootstrap::kSlotFirstDeclared + 4 + registry_rows +
                                nmspace_rows + i,
                            bound[i].port, seL4_CapRights_new(1, 0, 0, 1), 0,
                            0};
                    }
                    /* The windows as frame capabilities, two groups per port in
                     * the ports' own order -- the manager's own pages, then the
                     * set reserved for its children -- pages ascending within a
                     * group, and only the block ports' windows granted (the
                     * pairing above is 4 KiB a page; a scanout window's mega
                     * pages are not the storage stack's to hand out). All minted
                     * from pristine sets, so they arrive with
                     * no ASID and the child may map them (kernel/src/arch/
                     * riscv/kernel/vspace.c:869-878). */
                    uint32_t at = 0;
                    for (uint32_t i = 0; i < bound_count; ++i) {
                        if (!block_window(bound[i].name, bound[i].name_length)) {
                            continue;
                        }
                        for (uint32_t p = 0; p < bound[i].window_pages; ++p) {
                            frames[at] = {bound[i].window_physical +
                                              static_cast<uint64_t>(p) * 4096,
                                          4096, bound[i].window + p};
                            ++at;
                        }
                        for (uint32_t p = 0; p < bound[i].window_pages; ++p) {
                            frames[at] = {bound[i].window_physical +
                                              static_cast<uint64_t>(p) * 4096,
                                          4096, bound[i].children + p};
                            ++at;
                        }
                    }
                    aegir::spawn::Request request{};
                    request.name = kPartmgrName;
                    request.name_length = sizeof(kPartmgrName) - 1;
                    request.binary = kPartmgrBinary;
                    request.binary_length = sizeof(kPartmgrBinary) - 1;
                    request.account = "system";
                    request.account_length = 6;
                    request.priority = seL4_MaxPrio - 1;
                    request.ports = ports;
                    request.port_count = 4 + registry_rows + nmspace_rows + bound_count;
                    request.give_vspace = true;
                    /* The filesystem service's image, as bytes: the whole
                     * initrd is 1.2 MiB and does not fit a service-sized
                     * delegation, so what the manager starts is handed over
                     * one helper at a time (specs/services.md). One
                     * filesystem type exists today; the day a second does,
                     * what a partition's type calls for is a descriptor, not
                     * a recompile. */
                    static char const kFsBinary[] = "aegir-fs-fat";
                    uint64_t fs_binary_bytes = 0;
                    void const *fs_binary =
                        initrd.find(kFsBinary, sizeof(kFsBinary) - 1, &fs_binary_bytes);
                    request.devices = fs_binary;
                    request.devices_bytes = static_cast<uint32_t>(fs_binary_bytes);
                    request.untyped_physical = partmgr_physical;
                    request.untyped_bits = kPartmgrUntypedBits;
                    request.device_grants = frames;
                    request.device_grant_count = window_grant_count;
                    request.fault_endpoint = partmgr_fault;
                    request.badge = partmgr_badge;
                    aegir::spawn::Process process{};
                    if (!spawner.spawn(request, partmgr_account, process)) {
                        aegir::debug_write("      FAIL spawning partmgr: ");
                        aegir::debug_write(spawner.problem());
                        if (spawner.detail()[0] != '\0') {
                            aegir::debug_write(" (");
                            aegir::debug_write(spawner.detail());
                            aegir::debug_write(", seL4 error ");
                            aegir::debug_write_unsigned(spawner.error());
                            aegir::debug_write(")");
                        }
                        aegir::debug_write("\n");
                    } else {
                        aegir::debug_write("      spawned partmgr, badge ");
                        aegir::debug_write_unsigned(partmgr_badge);
                        aegir::debug_write("\n");
                        /* Its ready is not waited on here any more: the serve
                         * loop below receives it while answering the
                         * registry, which is what lets the manager *ask*
                         * before it reports. */
                        partmgr_running = true;
                        partmgr_supervision = process.supervision;
                    }
                }
            }

            /* The map is up and what it binds is running; from here the device
             * manager answers for it. It also still waits for the partition
             * manager's ready, and a blocked receive can wake for only one
             * object -- so the child's supervision notification (our half is
             * the receiving half; the child got the signaling half, Write
             * only) is *bound* to this thread, and one receive sees both
             * (kernel/manual/parts/notifications.tex:56-64).
             * (kernel/manual/parts/notifications.tex:56-64). The badge mark
             * that tells them apart is kCallMark, above. */
            if (registry_endpoint != 0) {
                seL4_Error bind_error = seL4_NoError;
                if (partmgr_running) {
                    bind_error = seL4_TCB_BindNotification(
                        aegir::bootstrap::kSlotOwnTcb, partmgr_supervision);
                }
                if (bind_error != seL4_NoError) {
                    write_line("FAIL", "the supervision notification would not bind");
                } else {
                    aegir::debug_write("      devmgr.registry: serving\n");
                    aegir::ipc::Owner registry(registry_endpoint);
                    /* The slot an `open` mints into: one, reused, because the
                     * reply transfers a copy and ours is deleted right after
                     * -- the shape vfs.namespace's resolve keeps. */
                    seL4_CPtr const mint_slot = g_objects.alloc_slot();
                    /* Ready is owed to the director once the partition manager
                     * reported its own -- or at once, when there is none. */
                    bool waiting_for_partmgr = partmgr_running;
                    if (!waiting_for_partmgr) {
                        seL4_Signal(aegir::bootstrap::kSlotSupervision);
                        write_line("device manager", "ready");
                    }
                    for (;;) {
                        seL4_Word badge = 0;
                        seL4_MessageInfo_t const info =
                            seL4_Recv(registry_endpoint, &badge);
                        if ((badge & kCallMark) == 0) {
                            /* A bare badge: a signal, not a call. */
                            if (waiting_for_partmgr && (badge & partmgr_badge) != 0) {
                                waiting_for_partmgr = false;
                                seL4_Signal(aegir::bootstrap::kSlotSupervision);
                                write_line("device manager", "ready");
                            }
                            continue;
                        }
                        uint32_t const length =
                            static_cast<uint32_t>(seL4_MessageInfo_get_length(info));
                        uint32_t const method = static_cast<uint32_t>(seL4_GetMR(0));
                        if (method == aegir::registry::kMethodCount) {
                            registry.reply(binding_count);
                        } else if (method == aegir::registry::kMethodDescribe &&
                                   length == 2 &&
                                   static_cast<uint64_t>(seL4_GetMR(1)) <
                                       binding_count) {
                            aegir::registry::Row row{};
                            fill_row(bindings[seL4_GetMR(1)], &row);
                            registry.reply_words(
                                reinterpret_cast<uint64_t const *>(&row),
                                aegir::registry::kRowWords);
                        } else if (method == aegir::registry::kMethodOpen && length == 2 &&
                                   static_cast<uint64_t>(seL4_GetMR(1)) < binding_count) {
                            /* The introduction: the bound driver's port,
                             * minted with the caller's own badge so the
                             * driver sees the true caller (specs/services.md).
                             * The unbadged originals are ours to mint from --
                             * an endpoint the spawner created may be minted
                             * again. An unbound row is the empty reply, the
                             * same as a row past the map. */
                            uint64_t const index =
                                static_cast<uint64_t>(seL4_GetMR(1));
                            seL4_CPtr port = 0;
                            if (bindings[index].spawned) {
                                for (uint32_t j = 0; j < bound_count; ++j) {
                                    if (bound[j].binding == index) {
                                        port = bound[j].port;
                                        break;
                                    }
                                }
                            }
                            if (port == 0 || mint_slot == 0 ||
                                seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, mint_slot,
                                                aegir::bootstrap::kCNodeBits,
                                                aegir::bootstrap::kSlotOwnCNode, port,
                                                aegir::bootstrap::kCNodeBits,
                                                /* Grant: the input protocol's
                                                 * subscribe rides a capability
                                                 * on the call (aegir/input.h),
                                                 * so an opened port may carry
                                                 * them. */
                                                seL4_CapRights_new(1, 1, 0, 1),
                                                badge) != seL4_NoError) {
                                registry.reply(0);
                            } else {
                                registry.reply_cap(nullptr, 0, mint_slot);
                                /* The kernel transferred a copy; ours leaves,
                                 * and the slot answers the next open. */
                                seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, mint_slot,
                                                  aegir::bootstrap::kCNodeBits);
                            }
                        } else if (method == aegir::registry::kMethodWindow &&
                                   length == 2 &&
                                   static_cast<uint64_t>(seL4_GetMR(1)) < binding_count) {
                            /* The window's shape, for a client that means to
                             * map it: page bits and page count. A row with no
                             * window -- or one nobody drives -- is the empty
                             * reply. */
                            uint64_t const index =
                                static_cast<uint64_t>(seL4_GetMR(1));
                            uint64_t shape[2] = {0, 0};
                            uint32_t shape_words = 0;
                            if (bindings[index].spawned) {
                                for (uint32_t j = 0; j < bound_count; ++j) {
                                    if (bound[j].binding == index &&
                                        bound[j].window_pages != 0) {
                                        shape[0] = bound[j].window_page_bits;
                                        shape[1] = bound[j].window_pages;
                                        shape_words = 2;
                                        break;
                                    }
                                }
                            }
                            registry.reply_words(shape, shape_words);
                        } else if (method == aegir::registry::kMethodWindowFrame &&
                                   length == 3 &&
                                   static_cast<uint64_t>(seL4_GetMR(1)) < binding_count) {
                            /* One frame of the window, from the pristine
                             * client set: a copy of a cap nobody has mapped
                             * is the receiver's to map (the mint sets above
                             * exist because a mapped cap's copies are pinned
                             * to its ASID). The transfer is the open shape:
                             * copy into the reused slot, reply, delete. */
                            uint64_t const index =
                                static_cast<uint64_t>(seL4_GetMR(1));
                            uint64_t const frame =
                                static_cast<uint64_t>(seL4_GetMR(2));
                            seL4_CPtr pristine = 0;
                            if (bindings[index].spawned) {
                                for (uint32_t j = 0; j < bound_count; ++j) {
                                    if (bound[j].binding == index &&
                                        frame < bound[j].window_pages) {
                                        pristine = bound[j].window +
                                                   static_cast<seL4_CPtr>(frame);
                                        break;
                                    }
                                }
                            }
                            if (pristine == 0 || mint_slot == 0 ||
                                seL4_CNode_Copy(aegir::bootstrap::kSlotOwnCNode, mint_slot,
                                                aegir::bootstrap::kCNodeBits,
                                                aegir::bootstrap::kSlotOwnCNode, pristine,
                                                aegir::bootstrap::kCNodeBits,
                                                seL4_AllRights) != seL4_NoError) {
                                registry.reply(0);
                            } else {
                                registry.reply_cap(nullptr, 0, mint_slot);
                                seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, mint_slot,
                                                  aegir::bootstrap::kCNodeBits);
                            }
                        } else {
                            /* A method we do not know, or an index past the
                             * map: the answer says so by saying nothing
                             * (aegir/registry.h). */
                            registry.reply(0);
                        }
                    }
                }
            }
        }
    }

    /* Ready: whoever spawned us can carry on, and the supervisor can tell
     * everyone else apart from us (specs/director.md). This is the path that
     * never made it to serving -- no memory, no tree, no registry file, no
     * endpoint -- and it stops rather than spins at somebody else's
     * priority. */
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("device manager", "ready (nothing to serve)");
    aegir::halt();
}
