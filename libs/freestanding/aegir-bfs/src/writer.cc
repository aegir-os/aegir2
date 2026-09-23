/*
 * Writing a Be File System volume.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/bfs/writer.h>

#include <aegir/bfs/bplustree.h>
#include <aegir/bfs/inode.h>

namespace aegir::bfs {

namespace {

/* A read view of an inode block, for the volume's stream reader. */
Inode inode_view(uint8_t const *block) noexcept
{
    Inode inode{};
    inode.mode = le32(block + inode::kMode);
    inode.mtime = le64_signed(block + inode::kLastModified);
    inode.size = le64_signed(block + inode::kData + data::kSize);
    inode.run = le_run(block + inode::kInodeNum);
    inode.parent = le_run(block + inode::kParent);
    inode_get_stream(block, inode.data);
    return inode;
}

uint32_t blocks_for(uint64_t bytes, uint32_t block_size) noexcept
{
    return static_cast<uint32_t>((bytes + block_size - 1) / block_size);
}

/* A fresh directory's tree: the header and one leaf holding dot and dotdot. */
void build_dir_tree(uint8_t *tree, uint64_t self_block,
                    uint64_t parent_block) noexcept
{
    uint32_t const total = kTreeNodeSize * 2;
    for (uint32_t i = 0; i < total; ++i) {
        tree[i] = 0;
    }
    TreeHeader header{};
    header.node_size = kTreeNodeSize;
    header.max_levels = 1;
    header.data_type = kTreeStringType;
    header.root = kTreeNodeSize;
    header.free_node = static_cast<uint64_t>(kNullLink);
    header.maximum = total;
    tree_header_build(tree, header);

    uint8_t *node = tree + kTreeNodeSize;
    put_le64(node + node::kLeftLink, static_cast<uint64_t>(kNullLink));
    put_le64(node + node::kRightLink, static_cast<uint64_t>(kNullLink));
    put_le64(node + node::kOverflowLink, static_cast<uint64_t>(kNullLink));
    put_le16(node + node::kKeyCount, 2);
    put_le16(node + node::kAllKeyLength, 3);
    node[node::kFixed + 0] = '.';
    node[node::kFixed + 1] = '.';
    node[node::kFixed + 2] = '.';
    uint32_t const lengths = key_align(node::kFixed + 3);
    put_le16(node + lengths, 1);
    put_le16(node + lengths + 2, 2);
    uint32_t const values = lengths + 2 * 2;
    put_le64(node + values, self_block);
    put_le64(node + values + 8, parent_block);
}

}  // namespace

bool Writer::open(Volume *volume) noexcept
{
    if (volume == nullptr || !volume->valid() || !volume->writable()) {
        return false;
    }
    volume_ = volume;
    if (!allocator_.open(volume)) {
        return false;
    }
    for (uint32_t i = 0; i < kMaxBlockSize; ++i) {
        zero_[i] = 0;
    }
    return true;
}

bool Writer::read_inode_block(uint64_t block, uint8_t *out) noexcept
{
    if (!volume_->read_block(block, out)) {
        return false;
    }
    return le32(out + inode::kMagic1) == kInodeMagic1 &&
           (le32(out + inode::kFlags) & kInodeInUse) != 0;
}

bool Writer::tree_header(uint64_t parent_block, uint8_t *stream,
                         uint32_t *node_size, uint64_t *root,
                         uint64_t *maximum) noexcept
{
    if (!read_inode_block(parent_block, inode_)) {
        return false;
    }
    inode_get_stream(inode_, stream);
    Inode const view = inode_view(inode_);
    uint8_t header[tree_header::kBytes];
    if (!volume_->read_stream(view, 0, header, sizeof(header))) {
        return false;
    }
    TreeHeader parsed{};
    if (!tree_header_parse(header, &parsed)) {
        return false;
    }
    *node_size = parsed.node_size;
    *root = parsed.root;
    *maximum = parsed.maximum;
    return true;
}

bool Writer::tree_edit(uint64_t parent_block, char const *name,
                       uint32_t name_length, uint64_t value, bool insert,
                       bool *existed) noexcept
{
    uint32_t node_size = 0;
    uint64_t root = 0;
    uint64_t maximum = 0;
    if (!tree_header(parent_block, stream_, &node_size, &root, &maximum)) {
        return false;
    }
    if (root + node_size > maximum || node_size > kMaxBlockSize) {
        return false;
    }
    Inode const view = inode_view(inode_);
    if (!volume_->read_stream(view, root, node_, node_size)) {
        return false;
    }
    NodeInfo info{};
    if (!node_info(node_, node_size, &info)) {
        return false;
    }
    if (info.overflow != kNullLink) {
        /* An internal root is a directory past one node: the split is the
         * next step, and a wrong write here would be corruption. */
        return false;
    }
    bool present = false;
    if (insert) {
        if (!node_insert(work_, node_size, node_, name, name_length, value, &present)) {
            return false;
        }
        if (present) {
            *existed = true;
            return true;
        }
    } else {
        if (!node_remove(work_, node_size, node_, name, name_length, &present)) {
            return false;
        }
        if (!present) {
            *existed = false;
            return true;
        }
    }
    if (!volume_->write_stream_raw(stream_, data::kBytes, root, work_, node_size)) {
        return false;
    }
    *existed = !insert;
    return true;
}

bool Writer::create(uint64_t parent_block, char const *name,
                    uint32_t name_length, uint32_t mode, int64_t time,
                    uint64_t *out_block) noexcept
{
    if (name_length == 0 || name_length > kMaxName) {
        return false;
    }
    if (!read_inode_block(parent_block, inode_)) {
        return false;
    }
    Run const parent_run = le_run(inode_ + inode::kInodeNum);

    Run inode_run{};
    if (!allocator_.allocate(1, &inode_run)) {
        return false;
    }
    uint64_t const block = volume_->to_block(inode_run);
    bool const directory = mode_is_directory(mode);
    if (directory) {
        mode |= kModeStrIndex;
    }

    Run tree_run{};
    bool have_tree = false;
    if (directory) {
        uint32_t const tree_blocks =
            blocks_for(kTreeNodeSize * 2, volume_->block_size());
        if (!allocator_.allocate(tree_blocks, &tree_run) ||
            tree_run.length < tree_blocks) {
            if (tree_run.length != 0) {
                (void)allocator_.free(tree_run);
            }
            (void)allocator_.free(inode_run);
            return false;
        }
        have_tree = true;
    }

    inode_build(inode_, volume_->block_size(), inode_run, parent_run, mode, time,
                name, name_length);
    if (directory) {
        for (uint32_t i = 0; i < data::kBytes; ++i) {
            stream_[i] = 0;
        }
        put_run(stream_ + data::kDirect, tree_run);
        put_le64(stream_ + data::kMaxDirectRange, kTreeNodeSize * 2);
        inode_set_stream(inode_, stream_, kTreeNodeSize * 2, time);
    }
    if (!volume_->write_block(block, inode_)) {
        if (have_tree) {
            (void)allocator_.free(tree_run);
        }
        (void)allocator_.free(inode_run);
        return false;
    }
    if (directory) {
        uint8_t tree[kTreeNodeSize * 2];
        build_dir_tree(tree, block, volume_->to_block(parent_run));
        if (!volume_->write_stream_raw(stream_, data::kBytes, 0, tree,
                                       sizeof(tree))) {
            (void)allocator_.free(tree_run);
            (void)allocator_.free(inode_run);
            return false;
        }
    }

    bool existed = false;
    if (!tree_edit(parent_block, name, name_length, block, true, &existed) ||
        existed) {
        if (have_tree) {
            (void)allocator_.free(tree_run);
        }
        (void)allocator_.free(inode_run);
        return false;
    }
    *out_block = block;
    return true;
}

bool Writer::dir_is_empty(uint64_t dir_block) noexcept
{
    if (!read_inode_block(dir_block, inode_)) {
        return false;
    }
    inode_get_stream(inode_, stream_);
    Inode const view = inode_view(inode_);
    uint8_t header[tree_header::kBytes];
    if (!volume_->read_stream(view, 0, header, sizeof(header))) {
        return false;
    }
    TreeHeader parsed{};
    if (!tree_header_parse(header, &parsed)) {
        return false;
    }
    if (!volume_->read_stream(view, parsed.root, node_, parsed.node_size)) {
        return false;
    }
    NodeInfo info{};
    if (!node_info(node_, parsed.node_size, &info) ||
        info.overflow != kNullLink) {
        return false;
    }
    if (info.count != 2) {
        return false;
    }
    char first[kMaxName];
    char second[kMaxName];
    uint32_t first_length = 0;
    uint32_t second_length = 0;
    uint64_t ignored = 0;
    if (!node_entry(node_, parsed.node_size, 0, first, &first_length, &ignored) ||
        !node_entry(node_, parsed.node_size, 1, second, &second_length, &ignored)) {
        return false;
    }
    return first_length == 1 && first[0] == '.' && second_length == 2 &&
           second[0] == '.' && second[1] == '.';
}

bool Writer::free_stream(uint8_t const *stream) noexcept
{
    uint32_t const per_block = volume_->block_size() / 8;
    for (uint32_t i = 0; i < data::kDirectCount; ++i) {
        Run const run = le_run(stream + data::kDirect + i * 8);
        if (run_is_zero(run)) {
            break;
        }
        if (!allocator_.free(run)) {
            return false;
        }
    }
    Run const indirect = le_run(stream + data::kIndirect);
    if (!run_is_zero(indirect)) {
        for (uint32_t b = 0; b < indirect.length; ++b) {
            uint64_t const at = volume_->to_block(indirect) + b;
            if (!volume_->read_block(at, node_)) {
                return false;
            }
            for (uint32_t j = 0; j < per_block; ++j) {
                Run const run = le_run(node_ + j * 8);
                if (run_is_zero(run)) {
                    break;
                }
                if (!allocator_.free(run)) {
                    return false;
                }
            }
        }
        (void)allocator_.free(indirect);
    }
    Run const double_indirect = le_run(stream + data::kDoubleIndirect);
    if (!run_is_zero(double_indirect)) {
        for (uint32_t b = 0; b < double_indirect.length; ++b) {
            uint64_t const at = volume_->to_block(double_indirect) + b;
            if (!volume_->read_block(at, node_)) {
                return false;
            }
            for (uint32_t j = 0; j < per_block; ++j) {
                Run const array = le_run(node_ + j * 8);
                if (run_is_zero(array)) {
                    break;
                }
                for (uint32_t c = 0; c < array.length; ++c) {
                    if (!volume_->read_block(volume_->to_block(array) + c, work_)) {
                        return false;
                    }
                    for (uint32_t k = 0; k < per_block; ++k) {
                        Run const run = le_run(work_ + k * 8);
                        if (run_is_zero(run)) {
                            break;
                        }
                        if (!allocator_.free(run)) {
                            return false;
                        }
                    }
                }
                (void)allocator_.free(array);
            }
        }
        (void)allocator_.free(double_indirect);
    }
    return true;
}

bool Writer::destroy(uint64_t block) noexcept
{
    if (!read_inode_block(block, inode_)) {
        return false;
    }
    inode_get_stream(inode_, stream_);
    Run const inode_run = le_run(inode_ + inode::kInodeNum);
    if (!free_stream(stream_)) {
        return false;
    }
    return allocator_.free(inode_run);
}

uint64_t Writer::stream_blocks(uint8_t const *stream) const noexcept
{
    uint64_t covered = 0;
    for (uint32_t i = 0; i < data::kDirectCount; ++i) {
        Run const run = le_run(stream + data::kDirect + i * 8);
        if (run_is_zero(run)) {
            return covered;
        }
        covered += run.length;
    }
    uint32_t const per_block = volume_->block_size() / 8;
    Run const indirect = le_run(stream + data::kIndirect);
    if (!run_is_zero(indirect)) {
        for (uint32_t b = 0; b < indirect.length; ++b) {
            if (!volume_->read_block(volume_->to_block(indirect) + b, node_)) {
                return covered;
            }
            for (uint32_t j = 0; j < per_block; ++j) {
                Run const run = le_run(node_ + j * 8);
                if (run_is_zero(run)) {
                    return covered;
                }
                covered += run.length;
            }
        }
    }
    return covered;
}

bool Writer::append_run(uint8_t *stream, Run const &run) noexcept
{
    uint32_t used = 0;
    for (; used < data::kDirectCount; ++used) {
        if (run_is_zero(le_run(stream + data::kDirect + used * 8))) {
            break;
        }
    }
    if (used < data::kDirectCount) {
        Run last{};
        if (used > 0) {
            last = le_run(stream + data::kDirect + (used - 1) * 8);
        }
        if (used > 0 && runs_contiguous(last, run)) {
            last.length = static_cast<uint16_t>(last.length + run.length);
            put_run(stream + data::kDirect + (used - 1) * 8, last);
        } else {
            put_run(stream + data::kDirect + used * 8, run);
        }
        uint64_t const max_direct =
            le64_signed(stream + data::kMaxDirectRange) +
            static_cast<uint64_t>(run.length) * volume_->block_size();
        put_le64(stream + data::kMaxDirectRange, max_direct);
        return true;
    }

    Run indirect = le_run(stream + data::kIndirect);
    if (run_is_zero(indirect)) {
        Run array{};
        if (!allocator_.allocate(kNumArrayBlocks, &array) || array.length == 0) {
            return false;
        }
        for (uint32_t b = 0; b < array.length; ++b) {
            if (!volume_->write_block(volume_->to_block(array) + b, zero_)) {
                (void)allocator_.free(array);
                return false;
            }
        }
        put_run(stream + data::kIndirect, array);
        put_le64(stream + data::kMaxIndirectRange,
                 le64_signed(stream + data::kMaxDirectRange));
        indirect = array;
    }

    uint32_t const per_block = volume_->block_size() / 8;
    for (uint32_t b = 0; b < indirect.length; ++b) {
        uint64_t const at = volume_->to_block(indirect) + b;
        if (!volume_->read_block(at, node_)) {
            return false;
        }
        for (uint32_t j = 0; j < per_block; ++j) {
            Run const slot = le_run(node_ + j * 8);
            if (!run_is_zero(slot)) {
                continue;
            }
            Run previous{};
            if (j > 0) {
                previous = le_run(node_ + (j - 1) * 8);
            }
            if (j > 0 && runs_contiguous(previous, run)) {
                previous.length = static_cast<uint16_t>(previous.length + run.length);
                put_run(node_ + (j - 1) * 8, previous);
            } else {
                put_run(node_ + j * 8, run);
            }
            if (!volume_->write_block(at, node_)) {
                return false;
            }
            uint64_t const max_indirect =
                le64_signed(stream + data::kMaxIndirectRange) +
                static_cast<uint64_t>(run.length) * volume_->block_size();
            put_le64(stream + data::kMaxIndirectRange, max_indirect);
            return true;
        }
    }
    return false;
}

bool Writer::grow_to(uint8_t *stream, uint64_t needed_blocks,
                     uint64_t *covered) noexcept
{
    while (*covered < needed_blocks) {
        uint64_t want = needed_blocks - *covered;
        if (want > 65535) {
            want = 65535;
        }
        Run run{};
        if (!allocator_.allocate(static_cast<uint32_t>(want), &run)) {
            return false;
        }
        uint64_t const at = volume_->to_block(run);
        for (uint32_t b = 0; b < run.length; ++b) {
            if (!volume_->write_block(at + b, zero_)) {
                (void)allocator_.free(run);
                return false;
            }
        }
        if (!append_run(stream, run)) {
            (void)allocator_.free(run);
            return false;
        }
        *covered += run.length;
    }
    return true;
}

bool Writer::zero_range(uint8_t *stream, uint64_t from, uint64_t to) noexcept
{
    uint64_t pos = from;
    while (pos < to) {
        uint64_t chunk = to - pos;
        if (chunk > kMaxBlockSize) {
            chunk = kMaxBlockSize;
        }
        if (!volume_->write_stream_raw(stream, data::kBytes, pos, zero_,
                                       static_cast<uint32_t>(chunk))) {
            return false;
        }
        pos += chunk;
    }
    return true;
}

bool Writer::trim_stream(uint8_t *stream, uint64_t new_blocks) noexcept
{
    uint64_t logical = 0;
    for (uint32_t i = 0; i < data::kDirectCount; ++i) {
        Run run = le_run(stream + data::kDirect + i * 8);
        if (run_is_zero(run)) {
            break;
        }
        if (logical >= new_blocks) {
            if (!allocator_.free(run)) {
                return false;
            }
            put_run(stream + data::kDirect + i * 8, Run{0, 0, 0});
            continue;
        }
        if (logical + run.length <= new_blocks) {
            logical += run.length;
            continue;
        }
        uint32_t const keep = static_cast<uint32_t>(new_blocks - logical);
        Run tail = run;
        tail.start = static_cast<uint16_t>(tail.start + keep);
        tail.length = static_cast<uint16_t>(tail.length - keep);
        if (!allocator_.free(tail)) {
            return false;
        }
        run.length = static_cast<uint16_t>(keep);
        put_run(stream + data::kDirect + i * 8, run);
        logical += keep;
    }

    uint32_t const per_block = volume_->block_size() / 8;
    Run indirect = le_run(stream + data::kIndirect);
    if (!run_is_zero(indirect)) {
        for (uint32_t b = 0; b < indirect.length; ++b) {
            uint64_t const at = volume_->to_block(indirect) + b;
            if (!volume_->read_block(at, node_)) {
                return false;
            }
            bool changed = false;
            for (uint32_t j = 0; j < per_block; ++j) {
                Run run = le_run(node_ + j * 8);
                if (run_is_zero(run)) {
                    break;
                }
                if (logical >= new_blocks) {
                    if (!allocator_.free(run)) {
                        return false;
                    }
                    put_run(node_ + j * 8, Run{0, 0, 0});
                    changed = true;
                    continue;
                }
                if (logical + run.length <= new_blocks) {
                    logical += run.length;
                    continue;
                }
                uint32_t const keep = static_cast<uint32_t>(new_blocks - logical);
                Run tail = run;
                tail.start = static_cast<uint16_t>(tail.start + keep);
                tail.length = static_cast<uint16_t>(tail.length - keep);
                if (!allocator_.free(tail)) {
                    return false;
                }
                run.length = static_cast<uint16_t>(keep);
                put_run(node_ + j * 8, run);
                changed = true;
                logical += keep;
            }
            if (changed && !volume_->write_block(at, node_)) {
                return false;
            }
        }
        if (logical <=
            static_cast<uint64_t>(le64_signed(stream + data::kMaxDirectRange)) /
                volume_->block_size()) {
            if (!allocator_.free(indirect)) {
                return false;
            }
            put_run(stream + data::kIndirect, Run{0, 0, 0});
            put_le64(stream + data::kMaxIndirectRange, 0);
        }
    }

    /* Recompute the direct range from what is left. */
    uint64_t direct_bytes = 0;
    for (uint32_t i = 0; i < data::kDirectCount; ++i) {
        Run const run = le_run(stream + data::kDirect + i * 8);
        if (run_is_zero(run)) {
            break;
        }
        direct_bytes += static_cast<uint64_t>(run.length) * volume_->block_size();
    }
    put_le64(stream + data::kMaxDirectRange, direct_bytes);
    if (run_is_zero(le_run(stream + data::kIndirect))) {
        put_le64(stream + data::kMaxIndirectRange, 0);
    }
    return true;
}

bool Writer::write(uint64_t inode_block, uint64_t offset, uint8_t const *bytes,
                   uint32_t length, int64_t time) noexcept
{
    if (!read_inode_block(inode_block, inode_)) {
        return false;
    }
    inode_get_stream(inode_, stream_);
    uint64_t const old_size = static_cast<uint64_t>(inode_size(inode_));
    uint64_t new_size = offset + length;
    if (new_size < old_size) {
        new_size = old_size;
    }
    uint64_t const needed =
        (new_size + volume_->block_size() - 1) / volume_->block_size();
    uint64_t covered = stream_blocks(stream_);
    if (needed > covered && !grow_to(stream_, needed, &covered)) {
        return false;
    }
    /* A gap between the old end and where this write lands is zeros, not
     * whatever the block last held. */
    if (offset > old_size && !zero_range(stream_, old_size, offset)) {
        return false;
    }
    if (!volume_->write_stream_raw(stream_, data::kBytes, offset, bytes, length)) {
        return false;
    }
    inode_set_stream(inode_, stream_, static_cast<int64_t>(new_size), time);
    return volume_->write_block(inode_block, inode_);
}

bool Writer::truncate(uint64_t inode_block, uint64_t size, int64_t time) noexcept
{
    if (!read_inode_block(inode_block, inode_)) {
        return false;
    }
    inode_get_stream(inode_, stream_);
    uint64_t const old_size = static_cast<uint64_t>(inode_size(inode_));
    uint64_t const new_blocks =
        (size + volume_->block_size() - 1) / volume_->block_size();
    uint64_t covered = stream_blocks(stream_);
    if (new_blocks > covered) {
        if (!grow_to(stream_, new_blocks, &covered)) {
            return false;
        }
    } else if (new_blocks < covered) {
        if (!trim_stream(stream_, new_blocks)) {
            return false;
        }
    }
    if (size > old_size && !zero_range(stream_, old_size, size)) {
        return false;
    }
    inode_set_stream(inode_, stream_, static_cast<int64_t>(size), time);
    return volume_->write_block(inode_block, inode_);
}

bool Writer::remove(uint64_t parent_block, char const *name,
                    uint32_t name_length) noexcept
{
    if (!read_inode_block(parent_block, inode_)) {
        return false;
    }
    Inode const parent = inode_view(inode_);
    uint64_t child = 0;
    if (!volume_->dir_find(parent, name, name_length, &child)) {
        return false;
    }
    if (!read_inode_block(child, inode_)) {
        return false;
    }
    bool const directory = mode_is_directory(inode_mode(inode_));
    if (directory && !dir_is_empty(child)) {
        return false;
    }
    bool present = false;
    if (!tree_edit(parent_block, name, name_length, 0, false, &present) ||
        !present) {
        return false;
    }
    return destroy(child);
}

bool Writer::rename(uint64_t parent_block, char const *from, uint32_t from_length,
                    char const *to, uint32_t to_length) noexcept
{
    if (!read_inode_block(parent_block, inode_)) {
        return false;
    }
    Inode const parent = inode_view(inode_);
    uint64_t child = 0;
    if (!volume_->dir_find(parent, from, from_length, &child)) {
        return false;
    }
    uint64_t existing = 0;
    if (volume_->dir_find(parent, to, to_length, &existing)) {
        return false;
    }
    bool present = false;
    if (!tree_edit(parent_block, from, from_length, 0, false, &present) ||
        !present) {
        return false;
    }
    bool existed = false;
    if (!tree_edit(parent_block, to, to_length, child, true, &existed) ||
        existed) {
        bool restored = false;
        (void)tree_edit(parent_block, from, from_length, child, true, &restored);
        return false;
    }
    return true;
}

}  // namespace aegir::bfs