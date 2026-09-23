/*
 * Reading a Be File System volume.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/bfs/volume.h>

#include <aegir/bfs/bplustree.h>

namespace aegir::bfs {

namespace {

constexpr uint32_t kSectorBytes = 512;

}  // namespace

bool name_equals(char const *a, uint32_t a_length, char const *b,
                 uint32_t b_length) noexcept
{
    if (a_length != b_length) {
        return false;
    }
    for (uint32_t i = 0; i < a_length; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

bool Volume::read_block(uint64_t block, uint8_t *out) const noexcept
{
    uint32_t const sectors = block_size() / kSectorBytes;
    for (uint32_t i = 0; i < sectors; ++i) {
        if (!read_(context_, block * sectors + i, out + i * kSectorBytes)) {
            return false;
        }
    }
    return true;
}

uint64_t Volume::to_block(Run const &run) const noexcept
{
    return (static_cast<uint64_t>(run.allocation_group) << ag_shift_) | run.start;
}

uint64_t Volume::run_bytes(Run const &run) const noexcept
{
    return static_cast<uint64_t>(run.length) * block_size();
}

bool Volume::open(ReadSector read, void *context, WriteSector write) noexcept
{
    read_ = read;
    write_ = write;
    context_ = context;
    valid_ = false;

    /* The superblock is the 512 bytes at offset 512: sector 1, whatever the
     * block size turns out to be. */
    if (!read_(context_, 1, array_)) {
        return false;
    }
    uint8_t const *sb = array_;
    if (le32(sb + superblock::kMagic1) != kMagic1 ||
        le32(sb + superblock::kMagic2) != kMagic2 ||
        le32(sb + superblock::kMagic3) != kMagic3 ||
        le32(sb + superblock::kByteOrder) != kByteOrderLittleEndian) {
        return false;
    }
    uint32_t const block_size = le32(sb + superblock::kBlockSize);
    uint32_t const block_shift = le32(sb + superblock::kBlockShift);
    uint32_t const inode_size = le32(sb + superblock::kInodeSize);
    uint32_t const ag_shift = le32(sb + superblock::kAgShift);
    uint32_t const num_ags = le32(sb + superblock::kNumAgs);
    uint32_t const blocks_per_ag = le32(sb + superblock::kBlocksPerAg);
    uint64_t const num_blocks = le64(sb + superblock::kNumBlocks);
    if (block_size == 0 || block_size > kMaxBlockSize ||
        block_size != inode_size || (1u << block_shift) != block_size ||
        ag_shift < 1 || ag_shift > 30 || num_ags < 1 || blocks_per_ag < 1 ||
        num_blocks < 10) {
        return false;
    }
    uint64_t const expected_groups =
        (num_blocks + (1ull << ag_shift) - 1) >> ag_shift;
    if (num_ags != expected_groups) {
        return false;
    }
    block_shift_ = block_shift;
    ag_shift_ = ag_shift;
    num_ags_ = num_ags;
    blocks_per_ag_ = blocks_per_ag;
    num_blocks_ = num_blocks;
    used_blocks_ = le64(sb + superblock::kUsedBlocks);
    root_ = le_run(sb + superblock::kRootDir);
    indices_ = le_run(sb + superblock::kIndices);
    for (uint32_t i = 0; i < sizeof(name_); ++i) {
        name_[i] = static_cast<char>(sb[i]);
    }
    for (uint32_t i = 0; i < kSuperblockBytes; ++i) {
        superblock_[i] = sb[i];
    }
    valid_ = true;
    return true;
}

bool Volume::write_block(uint64_t block, uint8_t const *in) const noexcept
{
    if (write_ == nullptr) {
        return false;
    }
    uint32_t const sectors = block_size() / kSectorBytes;
    for (uint32_t i = 0; i < sectors; ++i) {
        if (!write_(context_, block * sectors + i, in + i * kSectorBytes)) {
            return false;
        }
    }
    return true;
}

bool Volume::set_used_blocks(uint64_t used) noexcept
{
    used_blocks_ = used;
    put_le64(superblock_ + superblock::kUsedBlocks, used);
    return true;
}

bool Volume::set_flags(uint32_t flags) noexcept
{
    put_le32(superblock_ + superblock::kFlags, flags);
    return true;
}

bool Volume::flush_superblock() const noexcept
{
    if (write_ == nullptr) {
        return false;
    }
    return write_(context_, 1, superblock_);
}

bool Volume::read_inode(uint64_t block, Inode *out) const noexcept
{
    if (!valid_ || block >= num_blocks_ || !read_block(block, block_)) {
        return false;
    }
    if (le32(block_ + inode::kMagic1) != kInodeMagic1 ||
        (le32(block_ + inode::kFlags) & kInodeInUse) == 0 ||
        le32(block_ + inode::kInodeSize) != block_size()) {
        return false;
    }
    out->mode = le32(block_ + inode::kMode);
    out->mtime = le64_signed(block_ + inode::kLastModified);
    out->size = le64_signed(block_ + inode::kData + data::kSize);
    out->run = le_run(block_ + inode::kInodeNum);
    out->parent = le_run(block_ + inode::kParent);
    out->attributes = le_run(block_ + inode::kAttributes);
    for (uint32_t i = 0; i < data::kBytes; ++i) {
        out->data[i] = block_[inode::kData + i];
    }
    return true;
}

bool Volume::read_part(Run const &run, uint64_t skip, uint8_t *out,
                       uint32_t length) const noexcept
{
    uint64_t byte = to_block(run) * block_size() + skip;
    uint8_t *dst = out;
    uint32_t left = length;
    while (left > 0) {
        uint64_t const block = byte / block_size();
        uint32_t const within = static_cast<uint32_t>(byte % block_size());
        uint32_t chunk = block_size() - within;
        if (chunk > left) {
            chunk = left;
        }
        if (!read_block(block, block_)) {
            return false;
        }
        __builtin_memcpy(dst, block_ + within, chunk);
        dst += chunk;
        byte += chunk;
        left -= chunk;
    }
    return true;
}

bool Volume::write_part(Run const &run, uint64_t skip, uint8_t const *in,
                        uint32_t length) const noexcept
{
    uint64_t byte = to_block(run) * block_size() + skip;
    uint8_t const *src = in;
    uint32_t left = length;
    while (left > 0) {
        uint64_t const block = byte / block_size();
        uint32_t const within = static_cast<uint32_t>(byte % block_size());
        uint32_t chunk = block_size() - within;
        if (chunk > left) {
            chunk = left;
        }
        if (within == 0 && chunk == block_size()) {
            if (!write_block(block, src)) {
                return false;
            }
        } else {
            if (!read_block(block, block_)) {
                return false;
            }
            __builtin_memcpy(block_ + within, src, chunk);
            if (!write_block(block, block_)) {
                return false;
            }
        }
        src += chunk;
        byte += chunk;
        left -= chunk;
    }
    return true;
}

bool Volume::read_stream(Inode const &inode, uint64_t offset, uint8_t *out,
                         uint32_t length) const noexcept
{
    if (length == 0) {
        return true;
    }
    /* A hole reads as zeros; a covered range is filled below. */
    __builtin_memset(out, 0, length);

    uint64_t pos = 0;
    /* Fills the part of [offset, offset+length) that `run` covers, and reports
     * whether the read succeeded. */
    auto handle = [&](Run const &run, uint64_t run_start) -> bool {
        uint64_t const span = run_bytes(run);
        uint64_t const req_end = offset + length;
        uint64_t const run_end = run_start + span;
        uint64_t const lo = offset > run_start ? offset : run_start;
        uint64_t const hi = req_end < run_end ? req_end : run_end;
        if (lo >= hi) {
            return true;
        }
        uint32_t const n = static_cast<uint32_t>(hi - lo);
        return read_part(run, lo - run_start, out + (lo - offset), n);
    };

    uint8_t const *const stream = inode.data;

    for (uint32_t i = 0; i < data::kDirectCount; ++i) {
        Run const run = le_run(stream + data::kDirect + i * 8);
        if (run_is_zero(run)) {
            break;
        }
        if (!handle(run, pos)) {
            return false;
        }
        pos += run_bytes(run);
    }

    Run const indirect = le_run(stream + data::kIndirect);
    uint32_t const runs_per_block = block_size() / 8;
    if (!run_is_zero(indirect)) {
        for (uint32_t b = 0; b < indirect.length; ++b) {
            if (!read_block(to_block(indirect) + b, array_)) {
                return false;
            }
            for (uint32_t j = 0; j < runs_per_block; ++j) {
                Run const run = le_run(array_ + j * 8);
                if (run_is_zero(run)) {
                    break;
                }
                if (!handle(run, pos)) {
                    return false;
                }
                pos += run_bytes(run);
            }
        }
    }

    Run const double_indirect = le_run(stream + data::kDoubleIndirect);
    if (!run_is_zero(double_indirect)) {
        for (uint32_t b = 0; b < double_indirect.length; ++b) {
            if (!read_block(to_block(double_indirect) + b, array_)) {
                return false;
            }
            for (uint32_t j = 0; j < runs_per_block; ++j) {
                Run const array_run = le_run(array_ + j * 8);
                if (run_is_zero(array_run)) {
                    break;
                }
                for (uint32_t c = 0; c < array_run.length; ++c) {
                    if (!read_block(to_block(array_run) + c, array2_)) {
                        return false;
                    }
                    for (uint32_t k = 0; k < runs_per_block; ++k) {
                        Run const run = le_run(array2_ + k * 8);
                        if (run_is_zero(run)) {
                            break;
                        }
                        if (!handle(run, pos)) {
                            return false;
                        }
                        pos += run_bytes(run);
                    }
                }
            }
        }
    }
    return true;
}

bool Volume::write_stream_raw(uint8_t const *stream, uint32_t stream_size,
                              uint64_t offset, uint8_t const *in,
                              uint32_t length) const noexcept
{
    if (length == 0) {
        return true;
    }
    static_cast<void>(stream_size);

    uint64_t cursor = offset;
    uint32_t left = length;
    uint8_t const *src = in;
    /* Writes the part of [cursor, cursor+left) that `run` covers. */
    auto handle = [&](Run const &run, uint64_t run_start) -> bool {
        uint64_t const span = run_bytes(run);
        uint64_t const req_end = cursor + left;
        uint64_t const run_end = run_start + span;
        uint64_t const lo = cursor > run_start ? cursor : run_start;
        uint64_t const hi = req_end < run_end ? req_end : run_end;
        if (lo >= hi) {
            return true;
        }
        uint32_t const n = static_cast<uint32_t>(hi - lo);
        if (!write_part(run, lo - run_start, src + (lo - cursor), n)) {
            return false;
        }
        left -= n;
        cursor = hi;
        return true;
    };

    uint64_t pos = 0;
    for (uint32_t i = 0; i < data::kDirectCount; ++i) {
        Run const run = le_run(stream + data::kDirect + i * 8);
        if (run_is_zero(run)) {
            return left == 0;
        }
        if (!handle(run, pos)) {
            return false;
        }
        pos += run_bytes(run);
    }

    Run const indirect = le_run(stream + data::kIndirect);
    uint32_t const runs_per_block = block_size() / 8;
    if (!run_is_zero(indirect)) {
        for (uint32_t b = 0; b < indirect.length; ++b) {
            if (!read_block(to_block(indirect) + b, array_)) {
                return false;
            }
            for (uint32_t j = 0; j < runs_per_block; ++j) {
                Run const run = le_run(array_ + j * 8);
                if (run_is_zero(run)) {
                    return left == 0;
                }
                if (!handle(run, pos)) {
                    return false;
                }
                pos += run_bytes(run);
            }
        }
    }

    Run const double_indirect = le_run(stream + data::kDoubleIndirect);
    if (!run_is_zero(double_indirect)) {
        for (uint32_t b = 0; b < double_indirect.length; ++b) {
            if (!read_block(to_block(double_indirect) + b, array_)) {
                return false;
            }
            for (uint32_t j = 0; j < runs_per_block; ++j) {
                Run const array_run = le_run(array_ + j * 8);
                if (run_is_zero(array_run)) {
                    return left == 0;
                }
                for (uint32_t c = 0; c < array_run.length; ++c) {
                    if (!read_block(to_block(array_run) + c, array2_)) {
                        return false;
                    }
                    for (uint32_t k = 0; k < runs_per_block; ++k) {
                        Run const run = le_run(array2_ + k * 8);
                        if (run_is_zero(run)) {
                            return left == 0;
                        }
                        if (!handle(run, pos)) {
                            return false;
                        }
                        pos += run_bytes(run);
                    }
                }
            }
        }
    }
    return left == 0;
}

uint32_t Volume::node_key_lengths(uint8_t const *node, uint16_t count,
                                  uint16_t *lengths) const noexcept
{
    uint16_t const all_key_length = le16(node + node::kAllKeyLength);
    uint8_t const *at = node + key_align(node::kFixed + all_key_length);
    /* The stored array is cumulative offsets; a key's length is the step
     * between its entry and the one before (Haiku's KeyAt). */
    uint16_t previous = 0;
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t const cumulative = le16(at + i * 2);
        lengths[i] = static_cast<uint16_t>(cumulative - previous);
        previous = cumulative;
    }
    return key_align(node::kFixed + all_key_length) + count * 2;
}

bool Volume::node_key(uint8_t const *node, uint16_t count, uint16_t index,
                      char *out, uint32_t *length) const noexcept
{
    if (index >= count || count > 512) {
        return false;
    }
    uint16_t lengths[512];
    node_key_lengths(node, count, lengths);
    uint32_t at = node::kFixed;
    for (uint16_t i = 0; i < index; ++i) {
        at += lengths[i];
    }
    if (lengths[index] > kMaxName || at + lengths[index] > block_size()) {
        return false;
    }
    for (uint16_t i = 0; i < lengths[index]; ++i) {
        out[i] = static_cast<char>(node[at + i]);
    }
    *length = lengths[index];
    return true;
}

bool Volume::node_header(Inode const &dir, uint32_t *node_size, uint64_t *root,
                         uint64_t *maximum) const noexcept
{
    uint8_t header[tree_header::kBytes];
    if (!read_stream(dir, 0, header, sizeof(header))) {
        return false;
    }
    if (le32(header + tree_header::kMagic) != kTreeMagic) {
        return false;
    }
    *node_size = le32(header + tree_header::kNodeSize);
    *root = le64(header + tree_header::kRootNode);
    *maximum = le64(header + tree_header::kMaximumSize);
    if (*node_size == 0 || *node_size > kMaxBlockSize) {
        return false;
    }
    return true;
}

bool Volume::dir_find(Inode const &dir, char const *name, uint32_t length,
                      uint64_t *inode_block) const noexcept
{
    uint32_t node_size = 0;
    uint64_t offset = 0;
    uint64_t maximum = 0;
    if (!node_header(dir, &node_size, &offset, &maximum)) {
        return false;
    }
    for (uint32_t depth = 0; depth < 16; ++depth) {
        if (offset + node_size > maximum ||
            !read_stream(dir, offset, tree_, node_size)) {
            return false;
        }
        int64_t const overflow = le64_signed(tree_ + node::kOverflowLink);
        uint16_t const count = le16(tree_ + node::kKeyCount);
        if (count > 512) {
            return false;
        }
        uint16_t key_lengths[512];
        uint32_t const values_at = node_key_lengths(tree_, count, key_lengths);

        /* Under BFS's convention an internal node's key i is the greatest key
         * in child i, so the first key the name is not greater than names the
         * child to follow (or the overflow child past the last key). */
        uint16_t child = count;
        bool equal = false;
        for (uint16_t i = 0; i < count; ++i) {
            uint32_t at = node::kFixed;
            for (uint16_t k = 0; k < i; ++k) {
                at += key_lengths[k];
            }
            uint16_t const key_length = key_lengths[i];
            if (key_length > kMaxName) {
                return false;
            }
            int const order = key_compare(name, length,
                                          reinterpret_cast<char const *>(tree_ + at),
                                          key_length);
            if (order <= 0) {
                child = i;
                equal = order == 0;
                break;
            }
        }
        if (overflow == kNullLink) {
            if (!equal) {
                return false;
            }
            *inode_block = le64(tree_ + values_at + child * 8);
            return true;
        }
        offset = child == count
                     ? static_cast<uint64_t>(overflow)
                     : le64(tree_ + values_at + child * 8);
    }
    return false;
}

bool Volume::dir_entry(Inode const &dir, uint32_t index, char *name,
                       uint32_t *name_length, uint64_t *inode_block) const noexcept
{
    uint32_t node_size = 0;
    uint64_t offset = 0;
    uint64_t maximum = 0;
    if (!node_header(dir, &node_size, &offset, &maximum)) {
        return false;
    }
    /* Descend to the leftmost leaf: the first child of each internal node. */
    for (uint32_t depth = 0; depth < 16; ++depth) {
        if (offset + node_size > maximum ||
            !read_stream(dir, offset, tree_, node_size)) {
            return false;
        }
        if (le64_signed(tree_ + node::kOverflowLink) == kNullLink) {
            break;
        }
        uint16_t const count = le16(tree_ + node::kKeyCount);
        uint16_t key_lengths[512];
        uint32_t const values_at = node_key_lengths(tree_, count, key_lengths);
        offset = count > 0 ? le64(tree_ + values_at)
                           : static_cast<uint64_t>(le64_signed(tree_ + node::kOverflowLink));
    }
    /* Walk the leaves through their right links, counting entries. An empty
     * leaf -- a directory that lost entries it once held -- is skipped. */
    uint32_t remaining = index;
    uint64_t const max_nodes = maximum / node_size + 1;
    for (uint64_t walked = 0; walked <= max_nodes && offset + node_size <= maximum;
         ++walked) {
        if (!read_stream(dir, offset, tree_, node_size) ||
            le64_signed(tree_ + node::kOverflowLink) != kNullLink) {
            return false;
        }
        uint16_t const count = le16(tree_ + node::kKeyCount);
        if (remaining < count) {
            uint16_t key_lengths[512];
            uint32_t const values_at = node_key_lengths(tree_, count, key_lengths);
            return node_key(tree_, count, static_cast<uint16_t>(remaining), name,
                            name_length) &&
                   (*inode_block = le64(tree_ + values_at + remaining * 8), true);
        }
        remaining -= count;
        offset = le64(tree_ + node::kRightLink);
        if (offset == static_cast<uint64_t>(kNullLink) ||
            offset == static_cast<uint64_t>(kFreeLink)) {
            break;
        }
    }
    return false;
}

}  // namespace aegir::bfs
