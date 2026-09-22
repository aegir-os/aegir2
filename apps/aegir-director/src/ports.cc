/*
 * The port graph -- implementation. See src/ports.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "ports.h"

#include <aegir/bootstrap.h>
#include <aegir/ipc/port.h>

namespace aegir::director {

namespace {

bool is_space(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r';
}

/** Split a comma-separated manifest value into names, trimming around each.
 *  Returns how many there are; fills at most `room` of them. Counting first is
 *  how the graph avoids a capacity it would have had to guess. */
uint32_t split_names(manifest::View value, PortGraph::Name *out, uint32_t room) noexcept
{
    uint32_t count = 0;
    char const *cursor = value.data;
    char const *const end = value.data + value.length;
    while (cursor != nullptr && cursor < end) {
        while (cursor < end && (*cursor == ',' || is_space(*cursor))) {
            ++cursor;
        }
        char const *start = cursor;
        while (cursor < end && *cursor != ',') {
            ++cursor;
        }
        char const *stop = cursor;
        while (stop > start && is_space(stop[-1])) {
            --stop;
        }
        if (stop > start) {
            if (out != nullptr && count < room) {
                out[count] = PortGraph::Name{start, static_cast<uint32_t>(stop - start)};
            }
            ++count;
        }
    }
    return count;
}

bool same_name(PortGraph::Name left, PortGraph::Name right) noexcept
{
    if (left.length != right.length) {
        return false;
    }
    for (uint32_t i = 0; i < left.length; ++i) {
        if (left.data[i] != right.data[i]) {
            return false;
        }
    }
    return true;
}

/** Some ports need other rights than the rule below gives, and the manifest's
 *  format has no field for it yet; this table is the declaration until it
 *  grows one.
 *
 *  - `vfs.namespace` transfers capabilities (specs/vfs.md), which needs Grant
 *    on both halves: nothing transfers unless the cap the sender invokes has
 *    it (kernel/src/kernel/thread.c:212-218), and a reply that carries a cap
 *    inherits its grant from the owner's receiving half (kernel/manual/parts/
 *    ipc.tex, "Calling and Replying").
 *  - `vol.initrd` is *published* by its owner: the initrd service registers
 *    the volume with the VFS itself, and minting the unbadged caller half
 *    that registration carries takes a source cap with at least those
 *    rights.
 *  - `devmgr.registry` answers its `open` with a capability -- the bound
 *    driver's port, minted with the caller's badge -- so the receiving half
 *    needs Grant for the reply to carry it (kernel/manual/parts/ipc.tex,
 *    "Calling and Replying"), and every right such a mint derives, because
 *    a mint keeps only what the source holds. Its callers carry the call
 *    mark: the owner shares its receive with the supervision notification
 *    of the children it starts, and the mark is what tells a call from a
 *    signal (aegir/ipc/port.h's kCallMark). */
struct Rights {
    seL4_CapRights_t owner;
    seL4_CapRights_t caller;
};

bool name_is(PortGraph::Name name, char const *text, uint32_t length) noexcept
{
    if (name.length != length) {
        return false;
    }
    for (uint32_t i = 0; i < length; ++i) {
        if (name.data[i] != text[i]) {
            return false;
        }
    }
    return true;
}

Rights rights_for(PortGraph::Name name) noexcept
{
    if (name_is(name, "vfs.namespace", 13)) {
        /* The owner mints the union cap out of this endpoint (specs/
         * namespace.md), and a mint keeps only what its source holds, so the
         * owner half needs every right the client's callable copy does. */
        return Rights{seL4_AllRights, seL4_CapRights_new(1, 1, 0, 1)};
    }
    if (name_is(name, "vol.initrd", 10)) {
        return Rights{seL4_CapRights_new(1, 0, 1, 1), seL4_CapRights_new(1, 0, 0, 1)};
    }
    if (name_is(name, "devmgr.registry", 15)) {
        return Rights{seL4_AllRights, seL4_CapRights_new(1, 0, 0, 1)};
    }
    if (name_is(name, "console.gui", 11)) {
        /* frame's answer carries a capability (specs/console.md) -- the
         * namespace's shape, and the namespace's reason. The owner holds
         * everything the caller's mint derives: a mint keeps only what the
         * source holds. */
        return Rights{seL4_AllRights, seL4_CapRights_new(1, 1, 0, 1)};
    }
    if (name_is(name, "bureau.menu", 11)) {
        /* The owner reads: it receives the port's calls (specs/workbench.md).
         * The caller writes, and carries Grant because register transfers the
         * doorbell capability -- the namespace's reason, above. */
        return Rights{seL4_CanRead, seL4_CapRights_new(1, 1, 0, 1)};
    }
    return Rights{seL4_CanRead, seL4_CapRights_new(1, 0, 0, 1)};
}

/** The badge a caller half carries: the caller's own number, plus the call
 *  mark for the one port whose owner needs it to tell a call from a signal
 *  (aegir/ipc/port.h's kCallMark, and the rights table's comment above). */
seL4_Word caller_badge_for(PortGraph::Name name, seL4_Word caller) noexcept
{
    if (name_is(name, "devmgr.registry", 15)) {
        return caller | aegir::ipc::kCallMark;
    }
    return caller;
}

}  // namespace

PortGraph::PortGraph(mem::Allocator &allocator, mem::Arena &arena) noexcept
    : allocator_(allocator), arena_(arena), ports_(nullptr), own_names_(nullptr),
      need_names_(nullptr), own_offset_(nullptr), need_offset_(nullptr), own_count_(nullptr),
      need_count_(nullptr), need_port_(nullptr), grants_(nullptr), grant_offset_(nullptr),
      order_(nullptr), entry_count_(0), port_count_(0), problem_("no problem")
{
}

bool PortGraph::fail(char const *what) noexcept
{
    problem_ = what;
    return false;
}

bool PortGraph::build(manifest::Manifest const &manifest, mem::Account &account) noexcept
{
    entry_count_ = manifest.size();
    if (entry_count_ == 0) {
        return fail("the manifest declares no service");
    }

    own_offset_ = static_cast<uint32_t *>(arena_.allocate(sizeof(uint32_t) * entry_count_));
    need_offset_ = static_cast<uint32_t *>(arena_.allocate(sizeof(uint32_t) * entry_count_));
    own_count_ = static_cast<uint32_t *>(arena_.allocate(sizeof(uint32_t) * entry_count_));
    need_count_ = static_cast<uint32_t *>(arena_.allocate(sizeof(uint32_t) * entry_count_));
    grant_offset_ = static_cast<uint32_t *>(arena_.allocate(sizeof(uint32_t) * entry_count_));
    order_ = static_cast<uint32_t *>(arena_.allocate(sizeof(uint32_t) * entry_count_));
    if (own_offset_ == nullptr || need_offset_ == nullptr || own_count_ == nullptr ||
        need_count_ == nullptr || grant_offset_ == nullptr || order_ == nullptr) {
        return fail("no memory for the port graph");
    }

    /* Count first: the arrays are sized from the manifest itself rather than from
     * a maximum chosen here (project rule: capacity grows on demand). */
    uint32_t owns_total = 0;
    uint32_t needs_total = 0;
    uint32_t grants_total = 0;
    for (uint32_t i = 0; i < entry_count_; ++i) {
        own_offset_[i] = owns_total;
        need_offset_[i] = needs_total;
        grant_offset_[i] = grants_total;
        own_count_[i] = split_names(manifest[i].owns, nullptr, 0);
        need_count_[i] = split_names(manifest[i].needs, nullptr, 0);
        owns_total += own_count_[i];
        needs_total += need_count_[i];
        grants_total += own_count_[i] + need_count_[i];
    }
    if (owns_total == 0) {
        return fail("no service declares a port, so there is nothing to call");
    }

    own_names_ = static_cast<Name *>(arena_.allocate(sizeof(Name) * owns_total));
    need_names_ = static_cast<Name *>(arena_.allocate(sizeof(Name) * needs_total));
    ports_ = static_cast<Port *>(arena_.allocate(sizeof(Port) * owns_total));
    need_port_ = static_cast<uint32_t *>(arena_.allocate(sizeof(uint32_t) * needs_total));
    grants_ = static_cast<spawn::PortGrant *>(
        arena_.allocate(sizeof(spawn::PortGrant) * grants_total));
    if (own_names_ == nullptr || ports_ == nullptr || grants_ == nullptr ||
        (needs_total > 0 && (need_names_ == nullptr || need_port_ == nullptr))) {
        return fail("no memory for the port graph");
    }

    for (uint32_t i = 0; i < entry_count_; ++i) {
        if (split_names(manifest[i].owns, own_names_ + own_offset_[i], own_count_[i]) !=
            own_count_[i]) {
            return fail("a service's ports do not read the same way twice");
        }
        if (split_names(manifest[i].needs, need_names_ + need_offset_[i], need_count_[i]) !=
            need_count_[i]) {
            return fail("a service's needs do not read the same way twice");
        }
    }

    /* One owner per port. This is the rule the whole security story rests on: the
     * owner is the only reader, so nobody else can be handed a capability that
     * could take a call meant for it (specs/services.md). */
    port_count_ = owns_total;
    for (uint32_t i = 0; i < entry_count_; ++i) {
        for (uint32_t j = 0; j < own_count_[i]; ++j) {
            Name const name = own_names_[own_offset_[i] + j];
            ports_[own_offset_[i] + j] = Port{name, i, 0};
            for (uint32_t k = 0; k < own_offset_[i] + j; ++k) {
                if (same_name(ports_[k].name, name)) {
                    return fail("two services declare the same port");
                }
            }
        }
    }

    /* Every need resolves to an owned port, and nobody needs what they own. */
    for (uint32_t i = 0; i < entry_count_; ++i) {
        for (uint32_t j = 0; j < need_count_[i]; ++j) {
            Name const name = need_names_[need_offset_[i] + j];
            uint32_t const found = port_index(name);
            if (found == port_count_) {
                return fail("a service needs a port nobody declares it owns");
            }
            if (ports_[found].owner == i) {
                return fail("a service needs a port it owns itself");
            }
            need_port_[need_offset_[i] + j] = found;
        }
    }

    /* Creation order: an owner before its first consumer. Simple repeated passes
     * rather than a sort, because the graph is small and the check is the point:
     * if no entry can be placed, the needs are a cycle and nothing is created. */
    bool *placed = static_cast<bool *>(arena_.allocate(sizeof(bool) * entry_count_));
    if (placed == nullptr) {
        return fail("no memory for the creation order");
    }
    for (uint32_t i = 0; i < entry_count_; ++i) {
        placed[i] = false;
    }
    for (uint32_t next = 0; next < entry_count_; ++next) {
        bool progressed = false;
        for (uint32_t i = 0; i < entry_count_ && !progressed; ++i) {
            if (placed[i]) {
                continue;
            }
            bool ready = true;
            for (uint32_t j = 0; j < need_count_[i] && ready; ++j) {
                ready = placed[ports_[need_port_[need_offset_[i] + j]].owner];
            }
            if (ready) {
                order_[next] = i;
                placed[i] = true;
                progressed = true;
            }
        }
        if (!progressed) {
            return fail("the services' needs form a cycle, so none of them can start");
        }
    }

    /* Endpoints, one per port, held by us so that both sides can be given the
     * side they are entitled to without either handing the other anything. */
    for (uint32_t i = 0; i < port_count_; ++i) {
        seL4_Error error = seL4_NoError;
        ports_[i].endpoint = allocator_.alloc_object(seL4_EndpointObject, seL4_EndpointBits,
                                                     account, &error);
        if (ports_[i].endpoint == 0) {
            return fail("no memory for a port's endpoint");
        }
    }

    /* What each service holds, in slot order: the ports it owns first -- Read
     * only, because a reply travels on the kernel's reply capability and not on
     * the endpoint -- then the ones it may call, with Write and GrantReply. The
     * second half is the kernel's own requirement for a capability that may be
     * called (out/aegir/libsel4/include/interfaces/sel4_client.h:1202). The
     * ports that need other rights are named in rights_for, above. */
    for (uint32_t i = 0; i < entry_count_; ++i) {
        uint64_t slot = bootstrap::kSlotFirstDeclared;
        for (uint32_t j = 0; j < own_count_[i]; ++j) {
            Port const &port = ports_[own_offset_[i] + j];
            grants_[grant_offset_[i] + j] =
                spawn::PortGrant{port.name.data, port.name.length, slot, port.endpoint,
                                 rights_for(port.name).owner, 0};
            ++slot;
        }
        for (uint32_t j = 0; j < need_count_[i]; ++j) {
            Port const &port = ports_[need_port_[need_offset_[i] + j]];
            grants_[grant_offset_[i] + own_count_[i] + j] =
                spawn::PortGrant{port.name.data, port.name.length, slot, port.endpoint,
                                 rights_for(port.name).caller,
                                 caller_badge_for(port.name, i + 1)};
            ++slot;
        }
    }
    return true;
}

uint32_t PortGraph::port_index(Name name) const noexcept
{
    for (uint32_t i = 0; i < port_count_; ++i) {
        if (same_name(ports_[i].name, name)) {
            return i;
        }
    }
    return port_count_;
}

char const *PortGraph::port_name(unsigned index, uint32_t *length) const noexcept
{
    if (index >= port_count_) {
        return nullptr;
    }
    if (length != nullptr) {
        *length = ports_[index].name.length;
    }
    return ports_[index].name.data;
}

uint32_t PortGraph::port_owner(unsigned index) const noexcept
{
    return index < port_count_ ? ports_[index].owner : 0;
}

spawn::PortGrant const *PortGraph::grants(uint32_t entry) const noexcept
{
    return entry < entry_count_ ? &grants_[grant_offset_[entry]] : nullptr;
}

uint32_t PortGraph::grant_count(uint32_t entry) const noexcept
{
    return entry < entry_count_ ? own_count_[entry] + need_count_[entry] : 0;
}

}  // namespace aegir::director
