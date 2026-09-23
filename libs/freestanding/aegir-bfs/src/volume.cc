/*
 * Reading a Be File System volume.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/bfs/volume.h>

#include <aegir/bfs/attribute.h>
#include <aegir/bfs/bplustree.h>
#include <aegir/bfs/inode.h>
#include <aegir/bfs/journal.h>

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

bool Volume::read_block_now(uint64_t block, uint8_t *out) const noexcept
{
    uint32_t const sectors = block_size() / kSectorBytes;
    for (uint32_t i = 0; i < sectors; ++i) {
        if (!read_(context_, block * sectors + i, out + i * kSectorBytes)) {
            return false;
        }
    }
    return true;
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
    log_ = le_run(sb + superblock::kLog);
    log_start_ = le64(sb + superblock::kLogStart);
    log_end_ = le64(sb + superblock::kLogEnd);
    for (uint32_t i = 0; i < sizeof(name_); ++i) {
        name_[i] = static_cast<char>(sb[i]);
    }
    for (uint32_t i = 0; i < kSuperblockBytes; ++i) {
        superblock_[i] = sb[i];
    }
    valid_ = true;
    return true;
}

uint64_t Volume::to_block(Run const &run) const noexcept
{
    return (static_cast<uint64_t>(run.allocation_group) << ag_shift_) | run.start;
}

Run Volume::to_run(uint64_t block) const noexcept
{
    Run run{};
    run.allocation_group = static_cast<uint32_t>(block >> ag_shift_);
    run.start = static_cast<uint16_t>(block & ((1ull << ag_shift_) - 1));
    run.length = 1;
    return run;
}

bool Volume::validate_run(Run const &run) const noexcept
{
    if (run.length == 0 || run.allocation_group >= num_ags_) {
        return false;
    }
    /* A run sits inside its allocation group's span (1 << ag_shift blocks),
     * as Haiku's ValidateBlockRun checks, and inside the volume. */
    uint64_t const span = 1ull << ag_shift_;
    if (static_cast<uint64_t>(run.start) + run.length > span) {
        return false;
    }
    return to_block(run) + run.length <= num_blocks_;
}

bool Volume::set_log_start(uint64_t position) noexcept
{
    log_start_ = position;
    put_le64(superblock_ + superblock::kLogStart, position);
    return true;
}

bool Volume::set_log_end(uint64_t position) noexcept
{
    log_end_ = position;
    put_le64(superblock_ + superblock::kLogEnd, position);
    return true;
}

bool Volume::read_block(uint64_t block, uint8_t *out) const noexcept
{
    if (journal_ != nullptr && journal_->peek(block, out)) {
        return true;
    }
    return read_block_now(block, out);
}

bool Volume::write_block(uint64_t block, uint8_t const *in) const noexcept
{
    if (journal_ != nullptr && journal_->active()) {
        return journal_->log(block, in);
    }
    return write_block_now(block, in);
}

bool Volume::write_block_now(uint64_t block, uint8_t const *in) const noexcept
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
    /* While a transaction is open, the commit owns the superblock: it writes
     * the allocated count with the log cursors. Writing it mid-transaction
     * would put a half-done operation on the disk with no log to repair it. */
    if (journal_ != nullptr && journal_->active()) {
        return true;
    }
    return write_superblock_now();
}

bool Volume::write_superblock_now() const noexcept
{
    if (write_ == nullptr) {
        return false;
    }
    return write_(context_, 1, superblock_);
}

void Volume::save_superblock(uint8_t *out) const noexcept
{
    for (uint32_t i = 0; i < kSuperblockBytes; ++i) {
        out[i] = superblock_[i];
    }
}

void Volume::restore_superblock(uint8_t const *in) noexcept
{
    for (uint32_t i = 0; i < kSuperblockBytes; ++i) {
        superblock_[i] = in[i];
    }
    used_blocks_ = le64(superblock_ + superblock::kUsedBlocks);
    log_start_ = le64(superblock_ + superblock::kLogStart);
    log_end_ = le64(superblock_ + superblock::kLogEnd);
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
    out->type = le32(block_ + inode::kType);
    out->mtime = le64_signed(block_ + inode::kLastModified);
    out->size = le64_signed(block_ + inode::kData + data::kSize);
    out->run = le_run(block_ + inode::kInodeNum);
    out->parent = le_run(block_ + inode::kParent);
    out->attributes = le_run(block_ + inode::kAttributes);
    out->name_length = 0;
    (void)small_file_name(block_, le32(block_ + inode::kInodeSize), out->name,
                          sizeof(out->name), &out->name_length);
    for (uint32_t i = 0; i < data::kBytes; ++i) {
        out->data[i] = block_[inode::kData + i];
    }
    return true;
}

bool Volume::next_inode(uint64_t *block, Inode *out) const noexcept
{
    for (uint64_t at = *block; at < num_blocks_; ++at) {
        Inode inode{};
        /* An inode names its own block; a stale copy of one -- in the log, or
         * in a freed block not yet overwritten -- names another, and is not
         * an inode here. */
        if (read_inode(at, &inode) && to_block(inode.run) == at) {
            *out = inode;
            *block = at + 1;
            return true;
        }
    }
    *block = num_blocks_;
    return false;
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
                        uint32_t length, bool direct) const noexcept
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
            if (!(direct ? write_block_now(block, src)
                         : write_block(block, src))) {
                return false;
            }
        } else {
            if (!read_block(block, block_)) {
                return false;
            }
            __builtin_memcpy(block_ + within, src, chunk);
            if (!(direct ? write_block_now(block, block_)
                         : write_block(block, block_))) {
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
    return write_stream_impl(stream, stream_size, offset, in, length, false);
}

bool Volume::write_stream_direct(uint8_t const *stream, uint32_t stream_size,
                                 uint64_t offset, uint8_t const *in,
                                 uint32_t length) const noexcept
{
    return write_stream_impl(stream, stream_size, offset, in, length, true);
}

bool Volume::write_stream_impl(uint8_t const *stream, uint32_t stream_size,
                               uint64_t offset, uint8_t const *in,
                               uint32_t length, bool direct) const noexcept
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
        if (!write_part(run, lo - run_start, src + (lo - offset), n, direct)) {
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
                         uint64_t *maximum, uint32_t *data_type) const noexcept
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
    if (data_type != nullptr) {
        *data_type = le32(header + tree_header::kDataType);
    }
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

bool Volume::index_inode(char const *name, uint32_t length,
                         uint64_t *inode_block) const noexcept
{
    if (run_is_zero(indices_)) {
        return false;
    }
    Inode dir{};
    if (!read_inode(to_block(indices_), &dir)) {
        return false;
    }
    return dir_find(dir, name, length, inode_block);
}

bool Volume::index_walk(uint64_t index_block, uint8_t const *key,
                        uint32_t key_length, uint32_t *position,
                        uint64_t *inode_block) const noexcept
{
    Inode index{};
    if (!read_inode(index_block, &index)) {
        return false;
    }
    uint32_t node_size = 0;
    uint64_t offset = 0;
    uint64_t maximum = 0;
    uint32_t data_type = kTreeStringType;
    if (!node_header(index, &node_size, &offset, &maximum, &data_type)) {
        return false;
    }

    /* Descend to the leaf holding the key, as dir_find does, but the key
     * compares by the index's type. */
    uint64_t value = 0;
    bool found = false;
    for (uint32_t depth = 0; depth < 16 && !found; ++depth) {
        if (offset + node_size > maximum ||
            !read_stream(index, offset, tree_, node_size)) {
            return false;
        }
        int64_t const overflow = le64_signed(tree_ + node::kOverflowLink);
        uint16_t const count = le16(tree_ + node::kKeyCount);
        if (count > 512) {
            return false;
        }
        uint16_t key_lengths[512];
        uint32_t const values_at = node_key_lengths(tree_, count, key_lengths);
        uint16_t child = count;
        bool equal = false;
        for (uint16_t i = 0; i < count; ++i) {
            uint32_t at = node::kFixed;
            for (uint16_t k = 0; k < i; ++k) {
                at += key_lengths[k];
            }
            int const order =
                key_compare_typed(data_type, key, key_length, tree_ + at, key_lengths[i]);
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
            value = le64(tree_ + values_at + child * 8);
            found = true;
        } else {
            offset = child == count ? static_cast<uint64_t>(overflow)
                                    : le64(tree_ + values_at + child * 8);
        }
    }
    if (!found) {
        return false;
    }

    if (!link_is_duplicate(static_cast<int64_t>(value))) {
        if (*position != 0) {
            return false; /* a lone value: the walk's first call returns it */
        }
        *position = 1;
        *inode_block = value;
        return true;
    }
    if (link_type(static_cast<int64_t>(value)) != kDuplicateNode) {
        return false; /* a fragment node: not one Aegir writes */
    }
    uint64_t chain = link_offset(static_cast<int64_t>(value));
    for (uint32_t depth = 0; depth < 4096; ++depth) {
        if (!read_stream(index, chain, tree_, node_size)) {
            return false;
        }
        uint32_t const count = duplicate_count(tree_, node_size);
        if (*position < count) {
            if (!duplicate_value(tree_, node_size, *position, inode_block)) {
                return false;
            }
            ++*position;
            return true;
        }
        *position -= count;
        int64_t const right = le64_signed(tree_ + node::kRightLink);
        if (right == kNullLink) {
            return false;
        }
        chain = static_cast<uint64_t>(right);
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

bool Volume::attr_dir_inode(Inode const &inode, Inode *dir) const noexcept
{
    if (run_is_zero(inode.attributes)) {
        return false;
    }
    return read_inode(to_block(inode.attributes), dir);
}

bool Volume::inode_raw(Inode const &inode, uint32_t *inode_size) const noexcept
{
    if (!read_block(to_block(inode.run), block_)) {
        return false;
    }
    *inode_size = le32(block_ + inode::kInodeSize);
    return *inode_size <= kMaxBlockSize && *inode_size >= inode::kSmallData;
}

bool Volume::attr_inode(Inode const &inode, char const *name,
                        uint32_t name_length, uint64_t *attr_block) const noexcept
{
    Inode dir;
    if (!attr_dir_inode(inode, &dir) ||
        !dir_find(dir, name, name_length, attr_block)) {
        return false;
    }
    Inode attribute;
    if (!read_inode(*attr_block, &attribute) || !mode_is_attr(attribute.mode)) {
        return false;
    }
    return true;
}

bool Volume::attr_stat(Inode const &inode, char const *name,
                       uint32_t name_length, uint32_t *type,
                       uint64_t *size) const noexcept
{
    uint32_t inode_size = 0;
    if (inode_raw(inode, &inode_size)) {
        SmallAttribute entry{};
        if (small_find(block_, inode_size, name, name_length, &entry)) {
            *type = entry.type;
            *size = entry.data_length;
            return true;
        }
    }
    uint64_t attribute_block = 0;
    Inode attribute;
    if (!attr_inode(inode, name, name_length, &attribute_block) ||
        !read_inode(attribute_block, &attribute)) {
        return false;
    }
    *type = attribute.type;
    *size = static_cast<uint64_t>(attribute.size);
    return true;
}

bool Volume::attr_read(Inode const &inode, char const *name,
                       uint32_t name_length, uint64_t offset, uint8_t *out,
                       uint32_t *length) const noexcept
{
    uint32_t inode_size = 0;
    if (inode_raw(inode, &inode_size)) {
        SmallAttribute entry{};
        if (small_find(block_, inode_size, name, name_length, &entry)) {
            if (offset >= entry.data_length) {
                *length = 0;
                return true;
            }
            uint32_t got = *length;
            if (got > entry.data_length - static_cast<uint32_t>(offset)) {
                got = entry.data_length - static_cast<uint32_t>(offset);
            }
            for (uint32_t i = 0; i < got; ++i) {
                out[i] = entry.data[offset + i];
            }
            *length = got;
            return true;
        }
    }
    uint64_t attribute_block = 0;
    Inode attribute;
    if (!attr_inode(inode, name, name_length, &attribute_block) ||
        !read_inode(attribute_block, &attribute)) {
        return false;
    }
    if (offset > static_cast<uint64_t>(attribute.size)) {
        *length = 0;
        return true;
    }
    uint32_t got = *length;
    uint64_t const available = static_cast<uint64_t>(attribute.size) - offset;
    if (got > available) {
        got = static_cast<uint32_t>(available);
    }
    if (!read_stream(attribute, offset, out, got)) {
        return false;
    }
    *length = got;
    return true;
}

bool Volume::attr_entry(Inode const &inode, uint32_t index, char *name,
                        uint32_t *name_length, uint32_t *type,
                        uint64_t *size) const noexcept
{
    uint32_t inode_size = 0;
    uint32_t seen = 0;
    if (inode_raw(inode, &inode_size)) {
        uint32_t cursor = inode::kSmallData;
        SmallAttribute entry{};
        while (small_next(block_, inode_size, &cursor, &entry)) {
            if (seen == index) {
                for (uint32_t i = 0; i < entry.name_length; ++i) {
                    name[i] = entry.name[i];
                }
                *name_length = entry.name_length;
                *type = entry.type;
                *size = entry.data_length;
                return true;
            }
            ++seen;
        }
    }
    Inode dir;
    if (!attr_dir_inode(inode, &dir)) {
        return false;
    }
    uint64_t attribute_block = 0;
    if (!dir_entry(dir, index - seen, name, name_length, &attribute_block)) {
        return false;
    }
    Inode attribute;
    if (!read_inode(attribute_block, &attribute)) {
        return false;
    }
    *type = attribute.type;
    *size = static_cast<uint64_t>(attribute.size);
    return true;
}

}  // namespace aegir::bfs
