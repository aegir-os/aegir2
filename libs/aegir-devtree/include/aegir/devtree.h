/*
 * Reading the device tree.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The machine's devices are written down in a device tree blob, and the firmware
 * gave us one, so the first thing Aegir does with a device is read it rather than
 * probe for it (specs/services.md). This is a reader, not a parser of everything
 * the format allows: it walks the structure block, hands every node that says what
 * it is to a visitor, and stops there. What a node *means* is not this library's
 * business.
 *
 * The blob is read in place, so it must stay mapped while the tree is used, and
 * every string handed out points into it.
 */

#ifndef AEGIR_DEVTREE_H
#define AEGIR_DEVTREE_H

/* The C header, not <cstdint>: there is no C++ standard library here
 * (specs/build.md). */
#include <stdint.h>

namespace aegir::devtree {

/** One node that says what it is. */
struct Device {
    /** Index of this device within the walk, counting from zero. */
    uint32_t id;
    /** First `reg` entry: the register window's address and size. Zero when the
     *  node has no `reg`, or when the cells do not fit the fields below (a node
     *  bigger than 64 bits of address is not something this reader can describe
     *  in one entry, and saying zero is more honest than truncating). */
    uint64_t base;
    uint64_t size;
    /** First `interrupts` cell. Zero when the node has no `interrupts`. */
    uint32_t interrupt;
    /** True when the device has a register window at all. */
    bool has_region;
    /** True when the device has an interrupt. */
    bool has_interrupt;
    /** The first string of the node's `compatible` property, which is how a
     *  device is named in a tree ("virtio,mmio"), and its length. */
    char const *compatible;
    uint32_t compatible_length;
    /** The node's name without its unit address ("virtio_mmio"), and its length. */
    char const *name;
    uint32_t name_length;
};

/** A device tree blob that has been mapped and checked. */
class Tree {
public:
    /** Called for each node that has a `compatible` property, in the order the
     *  tree lists them. Returning false stops the walk. */
    struct Visitor {
        /* No virtual destructor on purpose: it would make the compiler emit a
         * deleting destructor, which needs `operator delete`, which a freestanding
         * program does not have (the link says so). Nothing deletes a visitor
         * through this interface -- the visitor belongs to the caller. */
        virtual bool device(Device const &device) = 0;
    };

    /** Take a blob of `available` readable bytes. False when it is not a device
     *  tree this reader understands. */
    bool adopt(void const *blob, uint64_t available) noexcept;

    /** Hand every device to `visitor`. False when the blob is malformed -- which
     *  includes a walk that ran off the end of the structure block, so a
     *  truncated blob cannot read past its own end. */
    bool walk(Visitor &visitor) const noexcept;

    /** Where a failed walk gave up, as a byte offset into the structure block,
     *  and how big the two blocks are. Diagnostics, because a tree that cannot be
     *  read is a fact about the blob and not something a caller can repair. */
    uint32_t failure_offset() const noexcept { return failure_offset_; }
    uint32_t struct_size() const noexcept { return struct_size_; }
    uint32_t strings_size() const noexcept { return strings_size_; }

    uint32_t version() const noexcept { return version_; }
    uint32_t total_size() const noexcept { return total_size_; }
    uint32_t reserved_entries() const noexcept { return reserved_entries_; }

private:
    uint8_t const *base_ = nullptr;
    uint32_t total_size_ = 0;
    uint32_t version_ = 0;
    uint32_t reserved_entries_ = 0;
    mutable uint32_t failure_offset_ = 0;
    uint32_t struct_offset_ = 0;
    uint32_t struct_size_ = 0;
    uint32_t strings_offset_ = 0;
    uint32_t strings_size_ = 0;
};

}  // namespace aegir::devtree

#endif  // AEGIR_DEVTREE_H
