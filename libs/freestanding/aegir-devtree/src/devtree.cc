/*
 * Reading the device tree.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The format is big-endian and self-describing (devicetree specification v0.4,
 * chapter 5): a header, a memory reservation block, a structure block of tokens,
 * and a strings block the structure block's property names index into. We walk the
 * structure block with a cursor that never leaves it, so a truncated or hostile
 * blob cannot make the reader touch memory it was not given -- which matters,
 * because until now the only thing that has read this blob is firmware.
 */

#include <aegir/devtree.h>

namespace aegir::devtree {
namespace {

/* Token types (devicetree specification, 5.4.1). */
constexpr uint32_t kTokenBeginNode = 1;
constexpr uint32_t kTokenEndNode = 2;
constexpr uint32_t kTokenProp = 3;
constexpr uint32_t kTokenNop = 4;
constexpr uint32_t kTokenEnd = 9;

constexpr uint32_t kMagic = 0xd00dfeedu;
/* The property layout this reader assumes; version 16 introduced it and later
 * versions have not changed it. */
constexpr uint32_t kMinimumVersion = 16;
/* Everything in the format is four-byte aligned, and the header is 40 bytes. */
constexpr uint32_t kHeaderBytes = 40;
/* The format's own defaults when a node does not say (specification, 2.3.5). */
constexpr uint32_t kDefaultAddressCells = 2;
constexpr uint32_t kDefaultSizeCells = 1;

uint32_t read_be32(uint8_t const *p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t read_be64(uint8_t const *p) noexcept {
    return (static_cast<uint64_t>(read_be32(p)) << 32) | read_be32(p + 4);
}

uint32_t align4(uint32_t value) noexcept { return (value + 3u) & ~static_cast<uint32_t>(3); }

/** The string at `text`, compared against `wanted`, never reading past `limit`. */
bool equals(char const *text, uint32_t limit, char const *wanted) noexcept {
    uint32_t i = 0;
    for (;; ++i) {
        if (i >= limit) {
            return false;
        }
        char const expect = wanted[i];
        if (text[i] != expect) {
            return false;
        }
        if (expect == '\0') {
            return true;
        }
    }
}

/** A cursor that cannot leave its block. */
struct Cursor {
    uint8_t const *p;
    uint8_t const *end;
};

bool take_u32(Cursor &cursor, uint32_t &value) noexcept {
    if (cursor.p + 4 > cursor.end) {
        return false;
    }
    value = read_be32(cursor.p);
    cursor.p += 4;
    return true;
}

bool skip(Cursor &cursor, uint32_t bytes) noexcept {
    if (cursor.p + bytes > cursor.end) {
        return false;
    }
    cursor.p += bytes;
    return true;
}

/** A NUL-terminated string padded to four bytes. */
bool take_string(Cursor &cursor, char const *&text, uint32_t &length) noexcept {
    uint8_t const *start = cursor.p;
    while (cursor.p < cursor.end && *cursor.p != '\0') {
        ++cursor.p;
    }
    if (cursor.p >= cursor.end) {
        return false;
    }
    length = static_cast<uint32_t>(cursor.p - start);
    uint8_t const *after = cursor.p + 1;
    uint32_t const padded = align4(static_cast<uint32_t>(after - start));
    if (start + padded > cursor.end) {
        return false;
    }
    text = reinterpret_cast<char const *>(start);
    cursor.p = start + padded;
    return true;
}

struct Property {
    uint32_t name_offset;
    uint8_t const *data;
    uint32_t length;
};

bool take_property(Cursor &cursor, Property &property) noexcept {
    uint32_t length = 0;
    uint32_t name_offset = 0;
    if (!take_u32(cursor, length) || !take_u32(cursor, name_offset)) {
        return false;
    }
    if (cursor.p + align4(length) > cursor.end) {
        return false;
    }
    property.name_offset = name_offset;
    property.data = cursor.p;
    property.length = length;
    return skip(cursor, align4(length));
}

/** What the walk carries between nodes. */
struct Walk {
    uint8_t const *block;   /* the structure block's first byte, for offsets */
    uint8_t const *strings;
    uint32_t strings_size;
    uint32_t next_id;
    bool stopped;
    Tree::Visitor *visitor;
};

/** Read up to two big-endian cells as one number. */
uint64_t read_cells(uint8_t const *p, uint32_t cells) noexcept {
    if (cells == 1) {
        return read_be32(p);
    }
    return read_be64(p);
}

void read_region(Property const &reg, uint32_t address_cells, uint32_t size_cells,
                 Device &device) noexcept {
    /* An entry is the parent's address cells followed by its size cells, and we
     * describe the first one. More than two cells of either is an address this
     * reader cannot put in a 64-bit field, so it says nothing rather than
     * something wrong. */
    if (address_cells == 0 || address_cells > 2 || size_cells > 2) {
        return;
    }
    uint32_t const words = address_cells + size_cells;
    if (reg.length < words * 4) {
        return;
    }
    device.base = read_cells(reg.data, address_cells);
    device.size = size_cells == 0 ? 0 : read_cells(reg.data + address_cells * 4, size_cells);
    device.has_region = true;
}

/** One node: its properties, then its children. */
bool walk_node(Cursor &cursor, uint32_t parent_address_cells, uint32_t parent_size_cells,
               Walk &walk) noexcept {
    char const *name = "";
    uint32_t name_length = 0;
    if (!take_string(cursor, name, name_length)) {
        return false;
    }

    char const *compatible = nullptr;
    uint32_t compatible_length = 0;
    Property reg{};
    Property interrupts{};
    bool have_reg = false;
    bool have_interrupts = false;
    uint32_t address_cells = kDefaultAddressCells;
    uint32_t size_cells = kDefaultSizeCells;
    /* Within a node, properties come before subnodes (devicetree specification
     * 5.4.1), so the first child's FDT_BEGIN_NODE is what ends the property
     * section -- and that token has already been read by the time we know. */
    bool have_child = false;

    for (;;) {
        uint32_t token = 0;
        if (!take_u32(cursor, token)) {
            return false;
        }
        if (token == kTokenEndNode) {
            break;
        }
        if (token == kTokenNop) {
            continue;
        }
        if (token == kTokenBeginNode) {
            have_child = true;
            break;
        }
        if (token != kTokenProp) {
            return false;
        }
        Property property{};
        if (!take_property(cursor, property)) {
            return false;
        }
        if (property.name_offset >= walk.strings_size) {
            return false;
        }
        char const *property_name = reinterpret_cast<char const *>(walk.strings + property.name_offset);
        uint32_t const room = walk.strings_size - property.name_offset;
        if (equals(property_name, room, "compatible")) {
            compatible = reinterpret_cast<char const *>(property.data);
            compatible_length = property.length;
        } else if (equals(property_name, room, "#address-cells")) {
            if (property.length >= 4) {
                address_cells = read_be32(property.data);
            }
        } else if (equals(property_name, room, "#size-cells")) {
            if (property.length >= 4) {
                size_cells = read_be32(property.data);
            }
        } else if (equals(property_name, room, "reg")) {
            reg = property;
            have_reg = true;
        } else if (equals(property_name, room, "interrupts")) {
            interrupts = property;
            have_interrupts = true;
        }
    }

    if (compatible != nullptr && !walk.stopped) {
        Device device{};
        device.id = walk.next_id++;
        device.compatible = compatible;
        /* `compatible` is a list of NUL-terminated strings: the first is the most
         * specific name, which is the one that says what the device is. */
        uint32_t first = 0;
        while (first < compatible_length && compatible[first] != '\0') {
            ++first;
        }
        device.compatible_length = first;
        device.name = name;
        uint32_t bare = 0;
        while (bare < name_length && name[bare] != '@') {
            ++bare;
        }
        device.name_length = bare;
        if (have_reg) {
            read_region(reg, parent_address_cells, parent_size_cells, device);
        }
        if (have_interrupts && interrupts.length >= 4) {
            device.interrupt = read_be32(interrupts.data);
            device.has_interrupt = true;
        }
        if (!walk.visitor->device(device)) {
            walk.stopped = true;
        }
    }

    /* No children: the FDT_END_NODE the property loop read is this node's own. */
    if (!have_child) {
        return true;
    }

    /* The first child, whose FDT_BEGIN_NODE the property loop consumed. */
    if (!walk_node(cursor, address_cells, size_cells, walk)) {
        return false;
    }
    for (;;) {
        if (walk.stopped) {
            return true;
        }
        uint32_t token = 0;
        if (!take_u32(cursor, token)) {
            return false;
        }
        if (token == kTokenEndNode) {
            return true;
        }
        if (token == kTokenNop) {
            continue;
        }
        if (token != kTokenBeginNode) {
            return false;
        }
        if (!walk_node(cursor, address_cells, size_cells, walk)) {
            return false;
        }
    }
}

}  // namespace

bool Tree::adopt(void const *blob, uint64_t available) noexcept {
    if (blob == nullptr || available < kHeaderBytes) {
        return false;
    }
    auto const *bytes = static_cast<uint8_t const *>(blob);
    if (read_be32(bytes) != kMagic) {
        return false;
    }
    uint32_t const total = read_be32(bytes + 4);
    if (total < kHeaderBytes || total > available) {
        return false;
    }
    uint32_t const version = read_be32(bytes + 20);
    if (version < kMinimumVersion) {
        return false;
    }
    uint32_t const struct_offset = read_be32(bytes + 8);
    uint32_t const strings_offset = read_be32(bytes + 12);
    uint32_t const struct_size = read_be32(bytes + 36);
    uint32_t const strings_size = read_be32(bytes + 32);
    if (struct_offset > total || struct_size > total - struct_offset) {
        return false;
    }
    if (strings_offset > total || strings_size > total - strings_offset) {
        return false;
    }

    /* The reservation block: pairs of 64-bit addresses and sizes, ending at a
     * zero pair. Counted rather than kept, because it is memory the firmware is
     * telling us not to use, and that is worth knowing about. */
    uint32_t reserved = 0;
    uint32_t const reserved_offset = read_be32(bytes + 16);
    if (reserved_offset != 0 && reserved_offset < total) {
        uint8_t const *p = bytes + reserved_offset;
        while (p + 16 <= bytes + total) {
            if (read_be64(p) == 0 && read_be64(p + 8) == 0) {
                break;
            }
            ++reserved;
            p += 16;
        }
    }

    base_ = bytes;
    total_size_ = total;
    version_ = version;
    reserved_entries_ = reserved;
    struct_offset_ = struct_offset;
    struct_size_ = struct_size;
    strings_offset_ = strings_offset;
    strings_size_ = strings_size;
    return true;
}

bool Tree::walk(Visitor &visitor) const noexcept {
    if (base_ == nullptr) {
        return false;
    }
    Cursor cursor{base_ + struct_offset_, base_ + struct_offset_ + struct_size_};
    uint32_t token = 0;
    if (!take_u32(cursor, token) || token != kTokenBeginNode) {
        return false;
    }
    uint8_t const *block = base_ + struct_offset_;
    Walk walk{block, base_ + strings_offset_, strings_size_, 0, false, &visitor};
    if (!walk_node(cursor, kDefaultAddressCells, kDefaultSizeCells, walk)) {
        failure_offset_ = static_cast<uint32_t>(cursor.p - block);
        return false;
    }
    /* A walk that stopped because the visitor said so is not malformed, and the
     * blob's terminator is not required to be reached once we have stopped. */
    return true;
}

}  // namespace aegir::devtree
