/*
 * The boot manifest: Aegir's service composition as data.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Format and fields are fixed in specs/services.md. Two rules from that spec are
 * load-bearing and live here rather than in the caller:
 *
 *   - Unknown keys and unknown sections are errors. A silently ignored line is
 *     how a typo turns an `authority` declaration into a service running with
 *     the wrong authority, so this parser refuses to be forgiving.
 *   - Views, not copies. A value points into the manifest text, which is an
 *     initrd entry living in our own image for the life of the process, so there
 *     is no string capacity to guess and nothing to free.
 *
 * Cross-entry checks (binaries that exist, ports that resolve, a spawn graph
 * that is acyclic) are director's, because they need the initrd and the whole
 * set at once (specs/services.md).
 */

#ifndef AEGIR_MANIFEST_H
#define AEGIR_MANIFEST_H

#include <aegir/mem/arena.h>
#include <stdint.h>

namespace aegir::manifest {

/** A range of the manifest text. Never owned, never NUL-terminated. */
struct View {
    char const *data;
    uint32_t length;
};

/** Compare a view with a NUL-terminated literal. */
bool equals(View view, char const *text) noexcept;

enum class Authority : uint8_t { System, User };

struct Entry {
    View name;
    View binary;
    View account;
    View authority_text;
    View owns;
    View needs;
    View grants;
    View spawns;
    View restart;
    View priority;
    View args;
    Authority authority;
    /* The service the machine's devices are given to, and its description of them.
     * A device is a capability, so it goes to exactly one service -- the one that
     * says so here, because that is where composition is declared
     * (specs/services.md). */
    bool device_manager;
    /* Which device this service is for, as the bus names it -- a virtio device id
     * (VIRTIO_ID_BLOCK is 2, VIRTIO_ID_NET is 1; projects/sel4_projects_libs
     * /libsel4vmmplatsupport/include/.../drivers/virtio.h). Zero means none: the
     * service is not about a device. */
    uint32_t device_id;
    /* Memory this service is given to retype objects out of, in KiB, or zero for none.
     * A driver needs it: a virtqueue's descriptor entries carry *physical* addresses, so
     * the service must own memory whose base the spawner can tell it -- and a service
     * cannot ask the kernel where its own memory is (specs/services.md,
     * specs/authority.md). A power of two, because a region that is carved is a power of
     * two wide; a request that is not one is rounded up when it is parsed. */
    uint32_t memory_kib;
    /* The untyped a spawning service is delegated, in MiB, or zero for the default
     * (services.cc's kDelegatedUntypedBits). Spawners' appetites diverge -- the
     * device manager holds drivers' windows and the partition manager's megabyte,
     * auth holds sessions -- so the manifest says who gets how much rather than a
     * shared constant saying it for everyone (specs/services.md,
     * specs/authority.md). A power of two, rounded up when parsed. */
    uint32_t delegate_mib;
    /* The flat initrd, mapped read-only, for a service that reads the boot
     *  image itself rather than spawning from it: the initrd service
     *  serves the archive as the Initrd: volume (specs/vfs.md). The same
     *  mapping a spawning service gets, without the spawn authority. */
    bool initrd;
    /* The service maps frames into its own address space -- the console's
     * pixel slices are the case (specs/console.md) -- so it is trusted with
     * its own VSpace root and a window of free addresses, the grant a
     * spawner gets without the spawn authority (aegir/spawn's
     * give_vspace). */
    bool maps;
    uint32_t line; /* the line the section started on, for messages */
};

class Manifest {
public:
    struct Problem {
        uint32_t line;
        char const *message;
    };

    Manifest(mem::Arena &arena, mem::Account &account) noexcept;

    /** Parse `text`; false means `problem()` says why. */
    bool parse(char const *text, uint32_t length) noexcept;

    uint32_t size() const noexcept { return count_; }
    Entry const &operator[](uint32_t index) const noexcept { return entries_[index]; }
    Problem problem() const noexcept { return problem_; }

    /** The version this parser understands. */
    static constexpr uint32_t kFormat = 1;

private:
    bool fail(uint32_t line, char const *message) noexcept;
    Entry *find(View name) noexcept;

    mem::Arena &arena_;
    mem::Account &account_;
    Entry *entries_;
    uint32_t count_;
    uint32_t capacity_;
    Problem problem_;
};

}  // namespace aegir::manifest

#endif  // AEGIR_MANIFEST_H
