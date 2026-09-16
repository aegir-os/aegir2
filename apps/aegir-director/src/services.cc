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
      graph_(allocator, arena), spawner_(allocator, scratch, arena, initrd)
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
              spawn::PortGrant const *extra, uint32_t extra_count) noexcept
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
        /* Memory the service asked for. It is carved here because the allocator is here and
         * the *address* is what matters: a region's physical base is the one thing a service
         * cannot find out for itself and a device has to be told, since a virtqueue's
         * descriptor entries are guest-physical addresses (specs/services.md,
         * specs/authority.md). */
        uint64_t memory_physical = 0;
        uint32_t memory_bits = 0;
        seL4_CPtr memory_cap = 0;
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
        }
        if ((entry.device_manager && extra_count > 0) || memory_cap != 0) {
            uint32_t const added =
                (entry.device_manager && extra_count > 0 ? extra_count : 0) + (memory_cap != 0 ? 1 : 0);
            auto *merged = static_cast<spawn::PortGrant *>(
                arena_.allocate(sizeof(spawn::PortGrant) * (grant_count + added)));
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
            if (memory_cap != 0) {
                /* Named `untyped`, which is what the service looks it up by, and given the
                 * size in bits with it: a port has no size and there is no invocation that
                 * reads an untyped's, so it has to be told (PortGrant::size_bits). */
                merged[at] = spawn::PortGrant{"untyped", 7, bootstrap::kSlotFirstDeclared + at,
                                              memory_cap, seL4_AllRights, 0, memory_bits};
                ++at;
            }
            grants = merged;
            grant_count += added;
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
