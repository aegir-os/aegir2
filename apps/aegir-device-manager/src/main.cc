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

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/devtree.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/vspace.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/spawn/initrd.h>
#include <aegir/spawn/process.h>
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
 *  what its instances are called. The registry is the code's knowledge of
 *  drivers; which of them the machine actually has is the tree's to say, and
 *  the table is deliberately small: adding a driver is adding a row, not a
 *  code path (specs/services.md). */
struct DriverRow {
    char const *compatible; /* the tree's name for the transport */
    uint32_t compatible_length;
    uint32_t virtio_id; /* 0: the compatible alone decides; else the registers must say this */
    char const *name_prefix; /* instances are prefix.busN: "blk" + "virtio" -> blk.virtio0 */
    uint32_t name_prefix_length;
    char const *bus;
    uint32_t bus_length;
    char const *binary;
    uint32_t binary_length;
    uint32_t memory_bits; /* the virtqueue it lays out */
};

DriverRow const kRegistry[] = {
    {"virtio,mmio", 11, 2u, "blk", 3, "virtio", 6, "aegir-virtio-blk", 16, 13u},
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
};

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
        for (uint32_t r = 0; r < sizeof(kRegistry) / sizeof(kRegistry[0]); ++r) {
            if (!compatible_is(device, kRegistry[r])) {
                continue;
            }
            if (fill != nullptr && count < capacity) {
                fill[count] = Binding{&kRegistry[r], device.base, 0,
                                      device.has_interrupt ? device.interrupt : 0, 0,
                                      nullptr, 0};
            }
            ++count;
            break;
        }
        return true;
    }

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

            /* The candidate cells: every tree device a registry row claims.
             * Count, then fill, so the array is exactly the tree's size. */
            MapBuilder counter;
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
                    aegir::debug_write(binding.row->compatible);
                    aegir::debug_write(" at ");
                    aegir::debug_write_hex(binding.base);
                    aegir::debug_write(": the tree describes it, but no frame was granted for it\n");
                    continue;
                }
                if (binding.row->virtio_id != 0) {
                    auto const *registers = static_cast<volatile uint32_t const *>(
                        g_scratch.map(binding.frame));
                    if (registers == nullptr) {
                        write_line("FAIL", "a device frame could not be mapped for probing");
                        binding.frame = 0;
                        continue;
                    }
                    uint32_t const probed_magic = registers[0x00 / 4];
                    uint32_t const probed_id = registers[0x08 / 4];
                    g_scratch.unmap(binding.frame);
                    if (probed_magic != 0x74726976u) {
                        binding.frame = 0;
                        continue;
                    }
                    if (probed_id != binding.row->virtio_id) {
                        aegir::debug_write("      map: virtio device ");
                        aegir::debug_write_unsigned(probed_id);
                        aegir::debug_write(" at ");
                        aegir::debug_write_hex(binding.base);
                        aegir::debug_write(": no driver in the registry\n");
                        binding.frame = 0;
                        continue;
                    }
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
                aegir::debug_write(named);
                aegir::debug_write(": ");
                aegir::debug_write(row->compatible);
                aegir::debug_write(" at ");
                aegir::debug_write_hex(binding.base);
                if (binding.irq != 0) {
                    aegir::debug_write(" irq ");
                    aegir::debug_write_unsigned(binding.irq);
                }
                aegir::debug_write(" -> ");
                aegir::debug_write(row->binary);
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
            if (binding_count > 0 && !have_log) {
                write_line("FAIL", "no delegatable log.main was given");
            }
            /* Badges for the processes we start count from 256: the low badges are
             * director's boot set, and until the badge space is a designed thing,
             * a spawning service's children live in a range of their own
             * (specs/services.md). */
            for (uint32_t b = 0; have_log && b < binding_count; ++b) {
                Binding const &binding = bindings[b];
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

                aegir::spawn::PortGrant const ports[] = {
                    {aegir::log::kPortName, aegir::log::kPortNameLength,
                     aegir::bootstrap::kSlotFirstDeclared,
                     static_cast<seL4_CPtr>(log_slot), seL4_CapRights_new(1, 0, 0, 1),
                     256u + b, 0},
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
                request.port_count = 1;
                request.device_frame = binding.frame;
                request.device_bytes = binding.bytes;
                request.device_physical = binding.base;
                request.device_grants = devices;
                request.device_grant_count = 1;
                request.memory_frame = memory_frame;
                request.memory_bytes = 1u << driver->memory_bits;
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
                    aegir::debug_write(binding.name);
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
                aegir::debug_write(binding.name);
                aegir::debug_write(" for ");
                aegir::debug_write(driver->compatible);
                aegir::debug_write(" at ");
                aegir::debug_write_hex(binding.base);
                aegir::debug_write(", badge ");
                aegir::debug_write_unsigned(request.badge);
                aegir::debug_write("\n");
                /* Ready is a signal on the supervision notification, badged with who
                 * it is -- the same protocol director's boot uses, because a spawned
                 * process does not know who spawned it (specs/director.md). */
                seL4_Word ready_badge = 0;
                seL4_Wait(process.supervision, &ready_badge);
            }
        }
    }

    /* Ready: whoever spawned us can carry on, and the supervisor can tell
     * everyone else apart from us (specs/director.md). */
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    write_line("device manager", "ready");

    /* Nothing to serve yet. The map is built and the drivers it binds are
     * running; what it still does not have is a port anyone can ask it, so
     * until then it stops rather than spins at somebody else's priority. */
    aegir::debug_write("      device manager: the map is up; a port to serve it comes next\n");
    aegir::halt();
}
