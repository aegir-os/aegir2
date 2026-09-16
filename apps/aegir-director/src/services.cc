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
    : initrd_(initrd), spawner_(allocator, scratch, arena, initrd)
{
}

void Services::boot(manifest::Manifest const &manifest, mem::Account &account, Started *started,
                    Boot &boot) noexcept
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

    for (uint32_t i = 0; i < manifest.size(); ++i) {
        manifest::Entry const &entry = manifest[i];
        spawn::Request request{};
        request.name = entry.name.data;
        request.name_length = entry.name.length;
        request.binary = entry.binary.data;
        request.binary_length = entry.binary.length;
        request.account = entry.account.data;
        request.account_length = entry.account.length;
        request.priority = priority_for(entry);

        spawn::Process process{};
        if (!spawner_.spawn(request, account, process)) {
            boot.problem = spawner_.problem();
            return;
        }
        started[boot.started].name = entry.name.data;
        started[boot.started].name_length = entry.name.length;
        started[boot.started].supervision = process.supervision;
        started[boot.started].entry = process.entry;
        ++boot.started;
    }
}

}  // namespace aegir::director
