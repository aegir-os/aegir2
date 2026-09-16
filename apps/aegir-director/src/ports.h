/*
 * The port graph the manifest declares.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * specs/services.md says what a port is and who may hold which side; this turns
 * the manifest's `owns` and `needs` into that: named ports, one owner each, an
 * endpoint each, the slots a service will find them in, and the order services
 * have to be created in so that an owner is always running before its first
 * consumer.
 *
 * Nothing is created until everything validates. A half-built system whose ports
 * point at services that were never started is exactly the state that is hardest
 * to diagnose, so validation comes first and a failure leaves the machine as it
 * was.
 */

#ifndef AEGIR_DIRECTOR_PORTS_H
#define AEGIR_DIRECTOR_PORTS_H

#include <aegir/manifest.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/spawn/process.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::director {

class PortGraph {
public:
    /** A port name, as a view into the manifest text. */
    struct Name {
        char const *data;
        uint32_t length;
    };

    PortGraph(mem::Allocator &allocator, mem::Arena &arena) noexcept;

    /** Resolve, validate, and create one endpoint per port. `problem()` says why
     *  when this fails. */
    bool build(manifest::Manifest const &manifest, mem::Account &account) noexcept;

    /** Creation order: every port's owner before its first consumer. Entry
     *  indices, one per manifest entry. */
    uint32_t const *order() const noexcept { return order_; }

    unsigned port_count() const noexcept { return port_count_; }
    char const *port_name(unsigned index, uint32_t *length) const noexcept;
    uint32_t port_owner(unsigned index) const noexcept;

    /** What one entry holds, in slot order: first the ports it owns (which it
     *  reads) and then the ones it may call. */
    spawn::PortGrant const *grants(uint32_t entry) const noexcept;
    uint32_t grant_count(uint32_t entry) const noexcept;

    char const *problem() const noexcept { return problem_; }

private:
    struct Port {
        Name name;
        uint32_t owner;
        seL4_CPtr endpoint;
    };

    bool fail(char const *what) noexcept;
    uint32_t port_index(Name name) const noexcept;

    mem::Allocator &allocator_;
    mem::Arena &arena_;
    Port *ports_;
    Name *own_names_;
    Name *need_names_;
    uint32_t *own_offset_;
    uint32_t *need_offset_;
    uint32_t *own_count_;
    uint32_t *need_count_;
    uint32_t *need_port_;
    spawn::PortGrant *grants_;
    uint32_t *grant_offset_;
    uint32_t *order_;
    uint32_t entry_count_;
    uint32_t port_count_;
    char const *problem_;
};

}  // namespace aegir::director

#endif  // AEGIR_DIRECTOR_PORTS_H
