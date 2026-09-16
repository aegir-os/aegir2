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
    : allocator_(allocator), initrd_(initrd), fault_endpoint_(0), graph_(allocator, arena),
      spawner_(allocator, scratch, arena, initrd)
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
              uint32_t devices_bytes, seL4_CPtr device_frame,
              uint32_t device_bytes) noexcept
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
        request.ports = graph_.grants(i);
        request.port_count = graph_.grant_count(i);
        request.devices = devices;
        request.devices_bytes = devices_bytes;
        request.device_frame = device_frame;
        request.device_bytes = device_bytes;
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
