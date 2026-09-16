/*
 * Turning the manifest into processes -- implementation. See src/services.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "services.h"

namespace aegir::director {

namespace {

/** The manifest's priority, or the one just below ours: a supervisor that cannot
 *  preempt what it supervises is not supervising it. */
uint32_t priority_for(manifest::Entry const &entry) noexcept
{
    uint32_t value = 0;
    bool any = false;
    for (uint32_t i = 0; i < entry.priority.length; ++i) {
        char const digit = entry.priority.data[i];
        if (digit < '0' || digit > '9') {
            any = false;
            break;
        }
        value = value * 10 + static_cast<uint32_t>(digit - '0');
        any = true;
    }
    if (!any || value == 0 || value > seL4_MaxPrio) {
        return seL4_MaxPrio - 1;
    }
    return value;
}

bool declared_as_user(manifest::Entry const &entry) noexcept
{
    return entry.authority == manifest::Authority::User;
}

}  // namespace

Services::Services(mem::Allocator &allocator, mem::Scratch &scratch, mem::Arena &arena,
                   spawn::Initrd const &initrd) noexcept
    : allocator_(allocator), arena_(arena), initrd_(initrd), fault_endpoint_(0),
      graph_(allocator, arena),
      /* Director's own pool is the boot set's: delegation hands a service a pool
       * of its own, so only director-spawned address spaces come from the
       * initial one (specs/authority.md). */
      spawner_(allocator, scratch, arena, initrd, seL4_CapInitThreadASIDPool,
               seL4_CapInitThreadCNode, seL4_WordBits)
{
}

bool Services::prepare(mem::Account &account) noexcept
{
    seL4_Error error = seL4_NoError;
    fault_endpoint_ = allocator_.alloc_object(seL4_EndpointObject, seL4_EndpointBits, account,
                                              &error);
    return fault_endpoint_ != 0;
}

void Services::boot(manifest::Manifest const &manifest, mem::Account &account, Started *started,
                    Boot &boot, Supervisor *supervisor, void const *devices,
              uint32_t devices_bytes, Device const *bus, uint32_t bus_count,
              spawn::PortGrant const *extra, uint32_t extra_count,
              uint64_t extra_untyped_physical) noexcept
{
    boot.declared = manifest.size();
    boot.started = 0;
    boot.problem = "";

    /* Validate before creating anything (specs/services.md). The check that can
     * be made today is the one that matters most: nothing may name a binary the
     * image does not carry. */
    for (uint32_t i = 0; i < manifest.size(); ++i) {
        manifest::Entry const &entry = manifest[i];
        if (initrd_.find(entry.binary.data, entry.binary.length, nullptr) == nullptr) {
            boot.problem = "a service names a binary the initrd does not contain";
            return;
        }
        if (declared_as_user(entry)) {
            /* Nothing can be a user service at boot: there is no user yet, and
             * sessions are created after authentication (specs/authority.md). */
            boot.problem = "a boot service cannot be declared as a user service";
            return;
        }
        /* A `spawns` name that resolves to nothing is a typo that would only
         * surface as a driver that never starts, so it is checked here, where
         * the whole set is at hand (specs/services.md). */
        if (entry.spawns.length > 0) {
            bool resolves = false;
            for (uint32_t j = 0; j < manifest.size(); ++j) {
                manifest::Entry const &other = manifest[j];
                if (other.name.length == entry.spawns.length) {
                    bool same = true;
                    for (uint32_t k = 0; k < entry.spawns.length; ++k) {
                        if (other.name.data[k] != entry.spawns.data[k]) {
                            same = false;
                            break;
                        }
                    }
                    if (same) {
                        resolves = true;
                        break;
                    }
                }
            }
            if (!resolves) {
                boot.problem = "a service spawns a name the manifest does not declare";
                return;
            }
        }
    }

    /* The ports first: who owns what, who may call it, and the order that makes
     * an owner always run before its first consumer. Nothing is created until all
     * of it validates (src/ports.h). */
    if (!graph_.build(manifest, account)) {
        boot.problem = graph_.problem();
        return;
    }

    for (uint32_t step = 0; step < manifest.size(); ++step) {
        uint32_t const i = graph_.order()[step];
        manifest::Entry const &entry = manifest[i];
        /* A service another entry spawns is not director's to start: the device
         * manager starts its driver when it gets to it (specs/services.md), and
         * starting it twice is not a philosophical problem -- the second copy
         * would be given the device the first one already drives. */
        bool spawned_by_another = false;
        for (uint32_t j = 0; j < manifest.size() && !spawned_by_another; ++j) {
            manifest::Entry const &parent = manifest[j];
            if (parent.spawns.length != entry.name.length) {
                continue;
            }
            bool same = true;
            for (uint32_t k = 0; k < entry.name.length; ++k) {
                if (parent.spawns.data[k] != entry.name.data[k]) {
                    same = false;
                    break;
                }
            }
            if (same) {
                spawned_by_another = true;
            }
        }
        if (spawned_by_another) {
            continue;
        }
        spawn::Request request{};
        request.name = entry.name.data;
        request.name_length = entry.name.length;
        request.binary = entry.binary.data;
        request.binary_length = entry.binary.length;
        request.account = entry.account.data;
        request.account_length = entry.account.length;
        request.priority = priority_for(entry);
        spawn::PortGrant const *grants = graph_.grants(i);
        uint32_t grant_count = graph_.grant_count(i);
        /* Capabilities director delegates to a service go in beside the ports the
         * manifest declares. A pool is not a port and cannot come from the port
         * graph; it is installed the way a port is -- by name, which is how the child
         * finds it, and by slot, which is the layout's business and not the caller's
         * (specs/authority.md). The slots come after the service's own ports. */
        /* The memory the service asked for is carved, retyped into a page and mapped into the
         * child by the spawner -- the child is told the address it can write and the physical
         * base the device reads, and needs no authority of its own. The untyped stays behind:
         * a carved region that has been *split* cannot be given away (the kernel answers
         * `RevokeFirst`), which is what "a port could not be installed" turned out to mean. */
        uint64_t memory_physical = 0;
        uint32_t memory_bits = 0;
        seL4_CPtr memory_cap = 0;
        seL4_CPtr memory_frame = 0;
        if (entry.memory_kib > 0) {
            memory_bits = 10;
            while ((1u << (memory_bits - 10)) < entry.memory_kib) {
                ++memory_bits;
            }
            seL4_Error memory_error = seL4_NoError;
            memory_cap =
                allocator_.carve_untyped(memory_bits, account, &memory_error, &memory_physical);
            if (memory_cap == 0) {
                boot.problem = "no memory for a service that asked for some";
                return;
            }
            /* The authority is the untyped; what the spawner needs is a *frame* it can map
             * into the child, and the child needs to be told the address it landed at --
             * which is the untyped's physical base, since a page-sized untyped is one page
             * (specs/services.md). */
            /* As many pages as were granted, retyped one after another. The allocator hands
             * out consecutive slots (alloc_slot bumps a cursor), which is what lets the
             * spawner map them as `memory_frame + i`. The queue needs two: the used ring
             * sits a page after the rest of it. */
            uint32_t const pages = (1u << memory_bits) / 4096u;
            seL4_Error page_error = seL4_NoError;
            for (uint32_t i = 0; i < pages; ++i) {
                seL4_CPtr const frame = allocator_.carve_page(memory_cap, account, &page_error);
                if (frame == 0) {
                    boot.problem = "the memory a service asked for could not be turned into pages";
                    return;
                }
                if (i == 0) {
                    memory_frame = frame;
                }
            }
        }
        if ((entry.device_manager && extra_count > 0) || memory_cap != 0) {
            uint32_t added = entry.device_manager && extra_count > 0 ? extra_count : 0;
            /* A spawning service also gets an *unbadged* copy of every port its
             * children need to call: a badged endpoint cap cannot be minted again
             * (deriveCap refuses it -- that is what "a port could not be installed"
             * with seL4_IllegalOperation meant), so a service that hands a port on
             * must be given one it may badge itself. The name carries a "spawn:"
             * prefix, because which copy is which is not something the block should
             * make a reader guess. */
            uint32_t spawn_needs = 0;
            uint32_t spawned = manifest.size();
            if (entry.device_manager && entry.spawns.length > 0) {
                for (uint32_t j = 0; j < manifest.size(); ++j) {
                    manifest::Entry const &other = manifest[j];
                    if (other.name.length != entry.spawns.length) {
                        continue;
                    }
                    bool same = true;
                    for (uint32_t k = 0; k < entry.spawns.length; ++k) {
                        if (other.name.data[k] != entry.spawns.data[k]) {
                            same = false;
                            break;
                        }
                    }
                    if (same) {
                        spawned = j;
                        break;
                    }
                }
            }
            if (spawned < manifest.size()) {
                for (uint32_t g = 0; g < graph_.grant_count(spawned); ++g) {
                    if (graph_.grants(spawned)[g].badge != 0) {
                        ++spawn_needs;
                    }
                }
            }
            auto *merged = static_cast<spawn::PortGrant *>(
                arena_.allocate(sizeof(spawn::PortGrant) * (grant_count + added + spawn_needs)));
            if (merged == nullptr) {
                boot.problem = "no room to merge what a service is given";
                return;
            }
            uint32_t at = 0;
            for (uint32_t g = 0; g < grant_count; ++g) {
                merged[at++] = grants[g];
            }
            if (entry.device_manager && extra_count > 0) {
                for (uint32_t g = 0; g < extra_count; ++g) {
                    merged[at] = extra[g];
                    merged[at].slot = bootstrap::kSlotFirstDeclared + at;
                    ++at;
                }
            }
            if (spawned < manifest.size() && spawn_needs > 0) {
                static char const kSpawnPrefix[] = "spawn:";
                for (uint32_t g = 0; g < graph_.grant_count(spawned); ++g) {
                    spawn::PortGrant const &need = graph_.grants(spawned)[g];
                    if (need.badge == 0) {
                        continue;
                    }
                    auto *named = static_cast<char *>(
                        arena_.allocate(sizeof(kSpawnPrefix) - 1 + need.name_length));
                    if (named == nullptr) {
                        boot.problem = "no room to name a delegatable port";
                        return;
                    }
                    for (uint32_t c = 0; c < sizeof(kSpawnPrefix) - 1; ++c) {
                        named[c] = kSpawnPrefix[c];
                    }
                    for (uint32_t c = 0; c < need.name_length; ++c) {
                        named[sizeof(kSpawnPrefix) - 1 + c] = need.name[c];
                    }
                    merged[at] = spawn::PortGrant{
                        named,
                        static_cast<uint32_t>(sizeof(kSpawnPrefix) - 1) + need.name_length,
                        bootstrap::kSlotFirstDeclared + at,
                        need.capability, need.rights, 0, 0};
                    ++at;
                }
            }
            grants = merged;
            grant_count = at;
        }
        request.ports = grants;
        request.port_count = grant_count;
        /* A device is a capability, and its description is only useful with it, so
         * both go to the service that declares itself the device manager and to no
         * one else. Handing the frame to every service is what made the first child
         * claim it and every later one be refused (specs/services.md). */
        request.devices = entry.device_manager ? devices : nullptr;
        request.devices_bytes = entry.device_manager ? devices_bytes : 0;
        /* The device this service is *for*: its section names a bus device id and the survey
         * found the device that answers to it. Naming an id the bus does not have gets no
         * device, which the boot report shows -- a service given nothing is easier to see
         * than one given the wrong thing (specs/services.md). */
        Device const *mine = nullptr;
        if (entry.device_id != 0) {
            for (uint32_t d = 0; d < bus_count; ++d) {
                if (bus[d].id == entry.device_id) {
                    mine = &bus[d];
                    break;
                }
            }
        }
        request.device_frame = mine != nullptr ? mine->frame : 0;
        request.device_bytes = mine != nullptr ? 4096u : 0;
        request.device_physical = mine != nullptr ? mine->address : 0;
        request.untyped_physical = memory_physical;
        request.untyped_bits = memory_bits;
        /* A delegated untyped says where it is the same way (specs/authority.md):
         * the block's `untyped` entry is how the service learns both the size and
         * the physical base of the memory its objects come from. */
        if (entry.device_manager && extra_untyped_physical != 0) {
            request.untyped_physical = extra_untyped_physical;
            request.untyped_bits = 0;
            for (uint32_t g = 0; g < extra_count; ++g) {
                if (extra[g].size_bits != 0) {
                    request.untyped_bits = extra[g].size_bits;
                }
            }
        }
        /* A service that spawns is given what spawning takes (specs/services.md,
         * specs/authority.md): its own VSpace root (the spawner grants a window of
         * free addresses with it), a copy of the initrd to read images out of, and
         * the devices its children are for -- as *capabilities* rather than
         * mappings, because a device manager's job is to hand them on. */
        if (entry.device_manager && entry.spawns.length > 0) {
            request.give_vspace = true;
            request.binaries = initrd_.blob();
            request.binaries_bytes = static_cast<uint32_t>(initrd_.blob_size());
            auto *device_grants = static_cast<spawn::DeviceGrant *>(
                arena_.allocate(sizeof(spawn::DeviceGrant) * 2));
            if (device_grants == nullptr) {
                boot.problem = "no room to list what a spawning service is given";
                return;
            }
            uint32_t device_grant_count = 0;
            for (uint32_t j = 0; j < manifest.size(); ++j) {
                manifest::Entry const &child = manifest[j];
                if (child.name.length != entry.spawns.length) {
                    continue;
                }
                bool same = true;
                for (uint32_t k = 0; k < entry.spawns.length; ++k) {
                    if (child.name.data[k] != entry.spawns.data[k]) {
                        same = false;
                        break;
                    }
                }
                if (!same || child.device_id == 0) {
                    continue;
                }
                for (uint32_t d = 0; d < bus_count; ++d) {
                    if (bus[d].id == child.device_id) {
                        device_grants[device_grant_count++] =
                            spawn::DeviceGrant{bus[d].address, 4096u, bus[d].frame};
                        break;
                    }
                }
            }
            request.device_grants = device_grants;
            request.device_grant_count = device_grant_count;
        }
        request.memory_frame = memory_frame;
        request.memory_bytes = memory_frame != 0 ? (1u << memory_bits) : 0;
        request.fault_endpoint = fault_endpoint_;
        /* Badges count from one so that zero keeps meaning "nobody in
         * particular" -- which is what director itself looks like. */
        request.badge = i + 1;

        spawn::Process process{};
        if (!spawner_.spawn(request, account, process)) {
            boot.problem = spawner_.problem();
            return;
        }
        started[boot.started].name = entry.name.data;
        started[boot.started].name_length = entry.name.length;
        started[boot.started].supervision = process.supervision;
        started[boot.started].tcb = process.tcb;
        started[boot.started].badge = request.badge;
        started[boot.started].entry = process.entry;
        if (supervisor != nullptr) {
            supervisor->record(boot.started, request.badge, process.tcb, process.supervision,
                               entry.name.data, entry.name.length);
        }
        ++boot.started;
    }
}

}  // namespace aegir::director
