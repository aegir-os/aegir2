/*
 * The Be File System's B+tree nodes.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/bfs/bplustree.h>

namespace aegir::bfs {

namespace {

uint32_t key_lengths_at(uint8_t const *node) noexcept
{
    return key_align(node::kFixed + le16(node + node::kAllKeyLength));
}

bool key_at(uint8_t const *node, uint16_t count, uint16_t index,
            uint8_t const **key, uint16_t *length) noexcept
{
    if (index >= count) {
        return false;
    }
    /* The stored lengths are cumulative offsets (Haiku's KeyAt): a key's
     * start is the previous entry and its length the step between them. */
    uint32_t const lengths = key_lengths_at(node);
    uint16_t const end = le16(node + lengths + index * 2);
    uint16_t const start = index == 0 ? 0 : le16(node + lengths + (index - 1) * 2);
    *length = static_cast<uint16_t>(end - start);
    *key = node + node::kFixed + start;
    return true;
}

void copy_key(uint8_t *dst, uint32_t *at, uint8_t const *node, uint16_t count,
              uint16_t index, uint16_t *length) noexcept
{
    uint8_t const *key = nullptr;
    (void)key_at(node, count, index, &key, length);
    for (uint16_t i = 0; i < *length; ++i) {
        dst[*at + i] = key[i];
    }
    *at += *length;
}

}  // namespace

bool tree_header_parse(uint8_t const *bytes, TreeHeader *out) noexcept
{
    if (le32(bytes + tree_header::kMagic) != kTreeMagic) {
        return false;
    }
    out->node_size = le32(bytes + tree_header::kNodeSize);
    out->max_levels = le32(bytes + tree_header::kMaxLevels);
    out->data_type = le32(bytes + tree_header::kDataType);
    out->root = le64(bytes + tree_header::kRootNode);
    out->free_node = le64(bytes + tree_header::kFreeNode);
    out->maximum = le64(bytes + tree_header::kMaximumSize);
    return out->node_size != 0 && out->node_size <= kMaxBlockSize;
}

void tree_header_build(uint8_t *bytes, TreeHeader const &header) noexcept
{
    put_le32(bytes + tree_header::kMagic, kTreeMagic);
    put_le32(bytes + tree_header::kNodeSize, header.node_size);
    put_le32(bytes + tree_header::kMaxLevels, header.max_levels);
    put_le32(bytes + tree_header::kDataType, header.data_type);
    put_le64(bytes + tree_header::kRootNode, header.root);
    put_le64(bytes + tree_header::kFreeNode, header.free_node);
    put_le64(bytes + tree_header::kMaximumSize, header.maximum);
}

int key_compare(char const *a, uint32_t a_length, char const *b,
                uint32_t b_length) noexcept
{
    uint32_t const common = a_length < b_length ? a_length : b_length;
    for (uint32_t i = 0; i < common; ++i) {
        uint8_t const x = static_cast<uint8_t>(a[i]);
        uint8_t const y = static_cast<uint8_t>(b[i]);
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    if (a_length == b_length) {
        return 0;
    }
    return a_length < b_length ? -1 : 1;
}

int key_compare_typed(uint32_t data_type, uint8_t const *a, uint32_t a_length,
                      uint8_t const *b, uint32_t b_length) noexcept
{
    if (data_type == kTreeInt32Type && a_length == 4 && b_length == 4) {
        int32_t const x = static_cast<int32_t>(le32(a));
        int32_t const y = static_cast<int32_t>(le32(b));
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (data_type == kTreeUInt32Type && a_length == 4 && b_length == 4) {
        uint32_t const x = le32(a);
        uint32_t const y = le32(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (data_type == kTreeInt64Type && a_length == 8 && b_length == 8) {
        int64_t const x = le64_signed(a);
        int64_t const y = le64_signed(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (data_type == kTreeUInt64Type && a_length == 8 && b_length == 8) {
        uint64_t const x = le64(a);
        uint64_t const y = le64(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    return key_compare(reinterpret_cast<char const *>(a), a_length,
                       reinterpret_cast<char const *>(b), b_length);
}

bool node_info(uint8_t const *node, uint32_t node_size, NodeInfo *out) noexcept
{
    if (node_size < node::kFixed) {
        return false;
    }
    out->count = le16(node + node::kKeyCount);
    out->all_key_length = le16(node + node::kAllKeyLength);
    out->overflow = le64_signed(node + node::kOverflowLink);
    out->used = key_lengths_at(node) + out->count * (2 + 8);
    return out->used <= node_size;
}

bool node_find(uint8_t const *node, uint32_t node_size, uint32_t data_type,
               char const *name, uint32_t length, uint64_t *value) noexcept
{
    NodeInfo info{};
    if (!node_info(node, node_size, &info)) {
        return false;
    }
    for (uint16_t i = 0; i < info.count; ++i) {
        uint8_t const *key = nullptr;
        uint16_t key_length = 0;
        if (!key_at(node, info.count, i, &key, &key_length)) {
            return false;
        }
        if (key_compare_typed(data_type, key, key_length,
                              reinterpret_cast<uint8_t const *>(name),
                              length) == 0) {
            uint32_t const values = key_lengths_at(node) + info.count * 2;
            *value = le64(node + values + i * 8);
            return true;
        }
    }
    return false;
}

bool node_entry(uint8_t const *node, uint32_t node_size, uint16_t index,
                char *name, uint32_t *name_length, uint64_t *value) noexcept
{
    NodeInfo info{};
    if (!node_info(node, node_size, &info) || index >= info.count) {
        return false;
    }
    uint8_t const *key = nullptr;
    uint16_t key_length = 0;
    if (!key_at(node, info.count, index, &key, &key_length) ||
        key_length > kMaxName) {
        return false;
    }
    for (uint16_t i = 0; i < key_length; ++i) {
        name[i] = static_cast<char>(key[i]);
    }
    *name_length = key_length;
    uint32_t const values = key_lengths_at(node) + info.count * 2;
    *value = le64(node + values + index * 8);
    return true;
}

bool node_insert(uint8_t *work, uint32_t node_size, uint32_t data_type,
                 uint8_t const *node, char const *name, uint32_t length,
                 uint64_t value, bool *existed) noexcept
{
    if (length == 0 || length > kMaxKeyLength) {
        return false;
    }
    NodeInfo info{};
    if (!node_info(node, node_size, &info)) {
        return false;
    }
    /* Find the first entry not less than `name`. */
    uint16_t at_index = info.count;
    for (uint16_t i = 0; i < info.count; ++i) {
        uint8_t const *key = nullptr;
        uint16_t key_length = 0;
        if (!key_at(node, info.count, i, &key, &key_length)) {
            return false;
        }
        int const order = key_compare_typed(
            data_type, key, key_length,
            reinterpret_cast<uint8_t const *>(name), length);
        if (order >= 0) {
            at_index = i;
            if (order == 0) {
                /* Replace the value; the node keeps its shape. */
                for (uint32_t b = 0; b < node_size; ++b) {
                    work[b] = node[b];
                }
                uint32_t const values = key_lengths_at(node) + info.count * 2;
                put_le64(work + values + i * 8, value);
                *existed = true;
                return true;
            }
            break;
        }
    }
    uint32_t const new_all =
        info.all_key_length + static_cast<uint32_t>(length);
    uint32_t const new_used = key_align(node::kFixed + new_all) +
                              (info.count + 1) * (2 + 8);
    if (new_used > node_size) {
        return false;
    }
    for (uint32_t b = 0; b < node_size; ++b) {
        work[b] = 0;
    }
    put_le64(work + node::kLeftLink, le64(node + node::kLeftLink));
    put_le64(work + node::kRightLink, le64(node + node::kRightLink));
    put_le64(work + node::kOverflowLink, le64(node + node::kOverflowLink));
    put_le16(work + node::kKeyCount, static_cast<uint16_t>(info.count + 1));
    put_le16(work + node::kAllKeyLength, static_cast<uint16_t>(new_all));

    uint32_t at = node::kFixed;
    for (uint16_t i = 0; i < at_index; ++i) {
        uint16_t key_length = 0;
        copy_key(work, &at, node, info.count, i, &key_length);
    }
    for (uint32_t i = 0; i < length; ++i) {
        work[at + i] = static_cast<uint8_t>(name[i]);
    }
    at += length;
    for (uint16_t i = at_index; i < info.count; ++i) {
        uint16_t key_length = 0;
        copy_key(work, &at, node, info.count, i, &key_length);
    }

    uint32_t const lengths = key_align(node::kFixed + new_all);
    uint32_t const values = lengths + (info.count + 1) * 2;
    uint32_t inserted = 0;
    uint16_t cumulative = 0;
    for (uint16_t i = 0; i < info.count + 1; ++i) {
        if (i == at_index) {
            cumulative = static_cast<uint16_t>(cumulative + length);
            put_le16(work + lengths + i * 2, cumulative);
            put_le64(work + values + i * 8, value);
            ++inserted;
            continue;
        }
        uint16_t const old = static_cast<uint16_t>(i - inserted);
        uint8_t const *key = nullptr;
        uint16_t key_length = 0;
        if (!key_at(node, info.count, old, &key, &key_length)) {
            return false;
        }
        cumulative = static_cast<uint16_t>(cumulative + key_length);
        put_le16(work + lengths + i * 2, cumulative);
        uint32_t const old_values = key_lengths_at(node) + info.count * 2;
        put_le64(work + values + i * 8, le64(node + old_values + old * 8));
    }
    *existed = false;
    return true;
}

bool node_remove(uint8_t *work, uint32_t node_size, uint32_t data_type,
                 uint8_t const *node, char const *name, uint32_t length,
                 bool *removed) noexcept
{
    NodeInfo info{};
    if (!node_info(node, node_size, &info)) {
        return false;
    }
    uint16_t found = info.count;
    uint16_t remove_length = 0;
    for (uint16_t i = 0; i < info.count; ++i) {
        uint8_t const *key = nullptr;
        uint16_t key_length = 0;
        if (!key_at(node, info.count, i, &key, &key_length)) {
            return false;
        }
        if (key_compare_typed(data_type, key, key_length,
                              reinterpret_cast<uint8_t const *>(name),
                              length) == 0) {
            found = i;
            remove_length = key_length;
            break;
        }
    }
    if (found == info.count) {
        for (uint32_t b = 0; b < node_size; ++b) {
            work[b] = node[b];
        }
        *removed = false;
        return true;
    }
    uint32_t const new_all = info.all_key_length - remove_length;
    for (uint32_t b = 0; b < node_size; ++b) {
        work[b] = 0;
    }
    put_le64(work + node::kLeftLink, le64(node + node::kLeftLink));
    put_le64(work + node::kRightLink, le64(node + node::kRightLink));
    put_le64(work + node::kOverflowLink, le64(node + node::kOverflowLink));
    put_le16(work + node::kKeyCount, static_cast<uint16_t>(info.count - 1));
    put_le16(work + node::kAllKeyLength, static_cast<uint16_t>(new_all));

    uint32_t at = node::kFixed;
    for (uint16_t i = 0; i < info.count; ++i) {
        if (i == found) {
            continue;
        }
        uint16_t key_length = 0;
        copy_key(work, &at, node, info.count, i, &key_length);
    }

    uint32_t const lengths = key_align(node::kFixed + new_all);
    uint32_t const values = lengths + (info.count - 1) * 2;
    uint16_t out = 0;
    uint16_t cumulative = 0;
    for (uint16_t i = 0; i < info.count; ++i) {
        if (i == found) {
            continue;
        }
        uint8_t const *key = nullptr;
        uint16_t key_length = 0;
        if (!key_at(node, info.count, i, &key, &key_length)) {
            return false;
        }
        cumulative = static_cast<uint16_t>(cumulative + key_length);
        put_le16(work + lengths + out * 2, cumulative);
        uint32_t const old_values = key_lengths_at(node) + info.count * 2;
        put_le64(work + values + out * 8, le64(node + old_values + i * 8));
        ++out;
    }
    *removed = true;
    return true;
}

bool node_set_value(uint8_t *work, uint32_t node_size, uint8_t const *node,
                    uint16_t index, uint64_t value) noexcept
{
    NodeInfo info{};
    if (!node_info(node, node_size, &info) || index >= info.count) {
        return false;
    }
    for (uint32_t b = 0; b < node_size; ++b) {
        work[b] = node[b];
    }
    uint32_t const values = key_lengths_at(node) + info.count * 2;
    put_le64(work + values + index * 8, value);
    return true;
}

uint32_t duplicate_capacity(uint32_t node_size) noexcept
{
    uint32_t const base = node::kOverflowLink + 8; /* count, then the values */
    if (node_size <= base) {
        return 0;
    }
    uint32_t const fits = (node_size - base) / 8;
    return fits < kNumDuplicateValues ? fits : kNumDuplicateValues;
}

uint32_t duplicate_count(uint8_t const *node, uint32_t node_size) noexcept
{
    uint32_t const capacity = duplicate_capacity(node_size);
    uint32_t const counted = static_cast<uint32_t>(
        le64_signed(node + node::kOverflowLink));
    return counted < capacity ? counted : capacity;
}

bool duplicate_value(uint8_t const *node, uint32_t node_size, uint32_t index,
                     uint64_t *out) noexcept
{
    if (index >= duplicate_count(node, node_size)) {
        return false;
    }
    *out = le64(node + node::kOverflowLink + 8 + index * 8);
    return true;
}

void duplicate_build(uint8_t *node, uint32_t node_size, uint64_t left,
                     uint64_t right, uint64_t const *values,
                     uint32_t count) noexcept
{
    for (uint32_t i = 0; i < node_size; ++i) {
        node[i] = 0;
    }
    put_le64(node + node::kLeftLink, left);
    put_le64(node + node::kRightLink, right);
    put_le64(node + node::kOverflowLink, count);
    for (uint32_t i = 0; i < count; ++i) {
        put_le64(node + node::kOverflowLink + 8 + i * 8, values[i]);
    }
}

}  // namespace aegir::bfs