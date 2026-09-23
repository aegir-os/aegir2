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

/* Build a B+tree node image in key order. A leaf has an overflow link of -1;
 * an internal node's overflow link is its rightmost child. */
void build_node(uint8_t *out, uint32_t node_size, bool leaf, int64_t left,
                int64_t right, int64_t overflow, uint8_t const *const *keys,
                uint16_t const *key_lengths, uint64_t const *values,
                uint16_t count) noexcept
{
    for (uint32_t i = 0; i < node_size; ++i) {
        out[i] = 0;
    }
    put_le64(out + node::kLeftLink, static_cast<uint64_t>(left));
    put_le64(out + node::kRightLink, static_cast<uint64_t>(right));
    put_le64(out + node::kOverflowLink,
             leaf ? static_cast<uint64_t>(kNullLink) : static_cast<uint64_t>(overflow));
    put_le16(out + node::kKeyCount, count);
    uint32_t all = 0;
    for (uint16_t i = 0; i < count; ++i) {
        all += key_lengths[i];
    }
    put_le16(out + node::kAllKeyLength, static_cast<uint16_t>(all));
    uint32_t at = node::kFixed;
    for (uint16_t i = 0; i < count; ++i) {
        for (uint16_t k = 0; k < key_lengths[i]; ++k) {
            out[at + k] = keys[i][k];
        }
        at += key_lengths[i];
    }
    uint32_t const lengths = key_align(node::kFixed + all);
    for (uint16_t i = 0; i < count; ++i) {
        put_le16(out + lengths + i * 2, key_lengths[i]);
    }
    uint32_t const values_at = lengths + count * 2;
    for (uint16_t i = 0; i < count; ++i) {
        put_le64(out + values_at + i * 8, values[i]);
    }
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
    if (!volume_->read_stream(view, 0, hdr_, sizeof(hdr_))) {
        return false;
    }
    TreeHeader parsed{};
    if (!tree_header_parse(hdr_, &parsed)) {
        return false;
    }
    *node_size = parsed.node_size;
    *root = parsed.root;
    *maximum = parsed.maximum;
    return true;
}

bool Writer::node_read(uint64_t offset, uint32_t node_size, uint8_t *out) noexcept
{
    return volume_->read_stream(inode_view(inode_), offset, out, node_size);
}

bool Writer::node_write(uint64_t offset, uint32_t node_size,
                        uint8_t const *node) noexcept
{
    return volume_->write_stream_raw(stream_, data::kBytes, offset, node, node_size);
}

bool Writer::gather(uint8_t const *node, uint32_t node_size, uint16_t *count_out,
                    int64_t *overflow_out) noexcept
{
    NodeInfo info{};
    if (!node_info(node, node_size, &info) || info.count > kMaxNodeEntries) {
        return false;
    }
    uint32_t const lengths = key_align(node::kFixed + le16(node + node::kAllKeyLength));
    uint32_t const values_at = lengths + info.count * 2;
    uint32_t at = node::kFixed;
    for (uint16_t i = 0; i < info.count; ++i) {
        g_lengths_[i] = le16(node + lengths + i * 2);
        g_keys_[i] = node + at;
        at += g_lengths_[i];
        g_values_[i] = le64(node + values_at + i * 8);
    }
    *count_out = info.count;
    *overflow_out = info.overflow;
    return true;
}

bool Writer::header_write() noexcept
{
    return volume_->write_stream_raw(stream_, data::kBytes, 0, hdr_, sizeof(hdr_));
}

bool Writer::append_node(uint64_t parent_block, uint32_t node_size,
                         uint64_t *out_offset) noexcept
{
    uint64_t const offset = le64(hdr_ + tree_header::kMaximumSize);
    uint64_t const end = offset + node_size;
    uint32_t const needed = blocks_for(end, volume_->block_size());
    uint64_t covered = stream_blocks(stream_);
    if (needed > covered && !grow_to(stream_, needed, &covered)) {
        return false;
    }
    *out_offset = offset;
    put_le64(hdr_ + tree_header::kMaximumSize, end);
    inode_set_stream(inode_, stream_, static_cast<int64_t>(end), inode_mtime(inode_));
    if (!volume_->write_block(parent_block, inode_)) {
        return false;
    }
    return header_write();
}

bool Writer::insert_into(uint64_t offset, uint32_t node_size, char const *name,
                         uint32_t name_length, uint64_t value,
                         TreeSplit *out) noexcept
{
    out->split = false;
    if (!node_read(offset, node_size, node_)) {
        return false;
    }
    NodeInfo info{};
    if (!node_info(node_, node_size, &info)) {
        return false;
    }
    if (info.overflow == kNullLink) {
        bool existed = false;
        if (!node_insert(work_, node_size, node_, name, name_length, value, &existed)) {
            return split_leaf(node_, node_size, offset, name, name_length, value, out);
        }
        return node_write(offset, node_size, work_);
    }
    /* An internal node: the first key the name is not greater than names the
     * child to follow, or the overflow child past the last key. The node is
     * copied to parent_ first, because the recursion below reuses node_. */
    uint16_t count = info.count;
    for (uint32_t i = 0; i < node_size; ++i) {
        parent_[i] = node_[i];
    }
    uint16_t child_index = count;
    uint64_t child = 0;
    for (uint16_t i = 0; i < count; ++i) {
        char key[kMaxName];
        uint32_t key_length = 0;
        uint64_t child_value = 0;
        if (!node_entry(parent_, node_size, i, key, &key_length, &child_value)) {
            return false;
        }
        if (key_compare(name, name_length, key, key_length) <= 0) {
            child_index = i;
            child = child_value;
            break;
        }
    }
    if (child_index == count) {
        child = static_cast<uint64_t>(le64_signed(parent_ + node::kOverflowLink));
    }
    TreeSplit child_split{};
    if (!insert_into(child, node_size, name, name_length, value, &child_split)) {
        return false;
    }
    if (!child_split.split) {
        return true;
    }
    bool existed = false;
    if (node_insert(work_, node_size, parent_, child_split.separator,
                    child_split.separator_length, child_split.left, &existed)) {
        uint16_t const new_count = le16(work_ + node::kKeyCount);
        if (static_cast<uint16_t>(child_index + 1) < new_count) {
            uint32_t const lengths =
                key_align(node::kFixed + le16(work_ + node::kAllKeyLength));
            uint32_t const values_at = lengths + new_count * 2;
            put_le64(work_ + values_at + (child_index + 1) * 8, child_split.right);
        } else {
            put_le64(work_ + node::kOverflowLink, child_split.right);
        }
        return node_write(offset, node_size, work_);
    }
    return split_internal(parent_, node_size, offset, child_split.separator,
                          child_split.separator_length, child_split.left,
                          child_split.right, out);
}

bool Writer::split_leaf(uint8_t const *node, uint32_t node_size, uint64_t offset,
                        char const *name, uint32_t name_length, uint64_t value,
                        TreeSplit *out) noexcept
{
    uint16_t count = 0;
    int64_t overflow = 0;
    if (!gather(node, node_size, &count, &overflow) ||
        static_cast<uint32_t>(count) + 1 > kMaxNodeEntries) {
        return false;
    }
    auto &keys = g_keys_;
    auto &key_lengths = g_lengths_;
    auto &values = g_values_;
    auto &ck = c_keys_;
    auto &ckl = c_lengths_;
    auto &cv = c_values_;
    uint16_t pos = count;
    for (uint16_t i = 0; i < count; ++i) {
        if (key_compare(name, name_length, reinterpret_cast<char const *>(keys[i]),
                        key_lengths[i]) < 0) {
            pos = i;
            break;
        }
    }
    for (uint16_t i = 0; i < pos; ++i) {
        ck[i] = keys[i];
        ckl[i] = key_lengths[i];
        cv[i] = values[i];
    }
    ck[pos] = reinterpret_cast<uint8_t const *>(name);
    ckl[pos] = static_cast<uint16_t>(name_length);
    cv[pos] = value;
    for (uint16_t i = pos; i < count; ++i) {
        ck[i + 1] = keys[i];
        ckl[i + 1] = key_lengths[i];
        cv[i + 1] = values[i];
    }
    uint16_t const total = count + 1;
    uint16_t const m = total / 2;
    int64_t const old_left = le64_signed(node + node::kLeftLink);
    int64_t const old_right = le64_signed(node + node::kRightLink);

    uint64_t other = 0;
    if (!append_node(edit_parent_, node_size, &other)) {
        return false;
    }
    build_node(fresh_, node_size, true, old_left, static_cast<int64_t>(other),
               kNullLink, ck, ckl, cv, m);
    build_node(work_, node_size, true, static_cast<int64_t>(offset), old_right,
               kNullLink, ck + m, ckl + m, cv + m, static_cast<uint16_t>(total - m));
    if (!node_write(offset, node_size, fresh_) ||
        !node_write(other, node_size, work_)) {
        return false;
    }
    if (old_right != kNullLink && old_right != kFreeLink) {
        if (!node_read(static_cast<uint64_t>(old_right), node_size, node_)) {
            return false;
        }
        put_le64(node_ + node::kLeftLink, other);
        if (!node_write(static_cast<uint64_t>(old_right), node_size, node_)) {
            return false;
        }
    }
    out->split = true;
    out->separator_length = ckl[m - 1];
    for (uint16_t i = 0; i < ckl[m - 1]; ++i) {
        out->separator[i] = static_cast<char>(ck[m - 1][i]);
    }
    out->left = offset;
    out->right = other;
    return true;
}

bool Writer::split_internal(uint8_t const *node, uint32_t node_size, uint64_t offset,
                            char const *name, uint32_t name_length, uint64_t value,
                            uint64_t replace_with, TreeSplit *out) noexcept
{
    uint16_t count = 0;
    int64_t overflow = 0;
    if (!gather(node, node_size, &count, &overflow) ||
        static_cast<uint32_t>(count) + 1 > kMaxNodeEntries) {
        return false;
    }
    auto &keys = g_keys_;
    auto &key_lengths = g_lengths_;
    auto &values = g_values_;
    auto &ck = c_keys_;
    auto &ckl = c_lengths_;
    auto &cv = c_values_;
    uint16_t pos = count;
    for (uint16_t i = 0; i < count; ++i) {
        if (key_compare(name, name_length, reinterpret_cast<char const *>(keys[i]),
                        key_lengths[i]) < 0) {
            pos = i;
            break;
        }
    }
    for (uint16_t i = 0; i < pos; ++i) {
        ck[i] = keys[i];
        ckl[i] = key_lengths[i];
        cv[i] = values[i];
    }
    ck[pos] = reinterpret_cast<uint8_t const *>(name);
    ckl[pos] = static_cast<uint16_t>(name_length);
    cv[pos] = value;
    for (uint16_t i = pos; i < count; ++i) {
        ck[i + 1] = keys[i];
        ckl[i + 1] = key_lengths[i];
        cv[i + 1] = values[i];
    }
    uint16_t const total = count + 1; /* keys and values both */
    int64_t combined_overflow = overflow;
    if (static_cast<uint16_t>(pos + 1) <= total - 1) {
        cv[pos + 1] = replace_with;
    } else {
        combined_overflow = static_cast<int64_t>(replace_with);
    }

    uint16_t j = total / 2;
    if (j < 1) {
        j = 1;
    }
    if (j >= total) {
        j = total - 1;
    }
    int64_t const old_left = le64_signed(node + node::kLeftLink);
    int64_t const old_right = le64_signed(node + node::kRightLink);

    uint64_t other = 0;
    if (!append_node(edit_parent_, node_size, &other)) {
        return false;
    }
    build_node(fresh_, node_size, false, old_left, static_cast<int64_t>(other),
               static_cast<int64_t>(cv[j]), ck, ckl, cv, j);
    build_node(work_, node_size, false, static_cast<int64_t>(offset), old_right,
               combined_overflow, ck + j + 1, ckl + j + 1, cv + j + 1,
               static_cast<uint16_t>(total - j - 1));
    if (!node_write(offset, node_size, fresh_) ||
        !node_write(other, node_size, work_)) {
        return false;
    }
    if (old_right != kNullLink && old_right != kFreeLink) {
        if (!node_read(static_cast<uint64_t>(old_right), node_size, node_)) {
            return false;
        }
        put_le64(node_ + node::kLeftLink, other);
        if (!node_write(static_cast<uint64_t>(old_right), node_size, node_)) {
            return false;
        }
    }
    out->split = true;
    out->separator_length = ckl[j];
    for (uint16_t i = 0; i < ckl[j]; ++i) {
        out->separator[i] = static_cast<char>(ck[j][i]);
    }
    out->left = offset;
    out->right = other;
    return true;
}

bool Writer::write_new_root(uint64_t root_block, TreeSplit const &split,
                            uint32_t node_size) noexcept
{
    uint8_t const *keys[1] = {reinterpret_cast<uint8_t const *>(split.separator)};
    uint16_t const key_lengths[1] = {static_cast<uint16_t>(split.separator_length)};
    uint64_t const values[1] = {split.left};
    uint64_t new_root = 0;
    if (!append_node(edit_parent_, node_size, &new_root)) {
        return false;
    }
    build_node(fresh_, node_size, false, kNullLink, kNullLink,
               static_cast<int64_t>(split.right), keys, key_lengths, values, 1);
    if (!node_write(new_root, node_size, fresh_)) {
        return false;
    }
    put_le64(hdr_ + tree_header::kRootNode, new_root);
    uint32_t const levels = le32(hdr_ + tree_header::kMaxLevels);
    put_le32(hdr_ + tree_header::kMaxLevels, levels + 1);
    static_cast<void>(root_block);
    return header_write();
}

bool Writer::remove_into(uint64_t offset, uint32_t node_size, char const *name,
                         uint32_t name_length, bool *removed) noexcept
{
    if (!node_read(offset, node_size, node_)) {
        return false;
    }
    NodeInfo info{};
    if (!node_info(node_, node_size, &info)) {
        return false;
    }
    if (info.overflow == kNullLink) {
        bool present = false;
        if (!node_remove(work_, node_size, node_, name, name_length, &present)) {
            return false;
        }
        if (!present) {
            *removed = false;
            return true;
        }
        *removed = true;
        return node_write(offset, node_size, work_);
    }
    uint64_t child = 0;
    uint16_t child_index = info.count;
    for (uint16_t i = 0; i < info.count; ++i) {
        char key[kMaxName];
        uint32_t key_length = 0;
        uint64_t child_value = 0;
        if (!node_entry(node_, node_size, i, key, &key_length, &child_value)) {
            return false;
        }
        if (key_compare(name, name_length, key, key_length) <= 0) {
            child_index = i;
            child = child_value;
            break;
        }
    }
    if (child_index == info.count) {
        child = static_cast<uint64_t>(le64_signed(node_ + node::kOverflowLink));
    }
    return remove_into(child, node_size, name, name_length, removed);
}

bool Writer::tree_edit(uint64_t parent_block, char const *name,
                       uint32_t name_length, uint64_t value, bool insert,
                       bool *existed) noexcept
{
    uint32_t node_size = 0;
    uint64_t root = 0;
    uint64_t maximum = 0;
    if (!tree_header(parent_block, stream_, &node_size, &root, &maximum) ||
        node_size > kMaxBlockSize || root + node_size > maximum) {
        return false;
    }
    edit_parent_ = parent_block;
    if (insert) {
        uint64_t existing = 0;
        if (volume_->dir_find(inode_view(inode_), name, name_length, &existing)) {
            *existed = true;
            return true;
        }
        TreeSplit split{};
        if (!insert_into(root, node_size, name, name_length, value, &split)) {
            return false;
        }
        if (split.split) {
            if (!write_new_root(root, split, node_size)) {
                return false;
            }
        }
        *existed = false;
        return true;
    }
    bool removed = false;
    if (!remove_into(root, node_size, name, name_length, &removed)) {
        return false;
    }
    if (!removed) {
        *existed = false;
        return true;
    }
    *existed = true;
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
    /* Descend to the leftmost leaf, then walk the leaves counting entries: a
     * directory is empty when the only names it holds are dot and dotdot. */
    uint64_t offset = parsed.root;
    for (uint32_t depth = 0; depth < 16; ++depth) {
        if (offset + parsed.node_size > parsed.maximum ||
            !volume_->read_stream(view, offset, node_, parsed.node_size)) {
            return false;
        }
        if (le64_signed(node_ + node::kOverflowLink) == kNullLink) {
            break;
        }
        uint16_t const count = le16(node_ + node::kKeyCount);
        uint32_t const values_at =
            key_align(node::kFixed + le16(node_ + node::kAllKeyLength)) + count * 2;
        offset = count > 0 ? le64(node_ + values_at)
                           : static_cast<uint64_t>(le64_signed(node_ + node::kOverflowLink));
    }
    uint32_t entries = 0;
    bool ok = true;
    uint64_t const max_nodes = parsed.maximum / parsed.node_size + 1;
    for (uint64_t walked = 0;
         walked <= max_nodes && offset + parsed.node_size <= parsed.maximum;
         ++walked) {
        if (!volume_->read_stream(view, offset, node_, parsed.node_size) ||
            le64_signed(node_ + node::kOverflowLink) != kNullLink) {
            return false;
        }
        uint16_t const count = le16(node_ + node::kKeyCount);
        for (uint16_t i = 0; i < count; ++i) {
            char key[kMaxName];
            uint32_t key_length = 0;
            uint64_t ignored = 0;
            if (!node_entry(node_, parsed.node_size, i, key, &key_length, &ignored)) {
                return false;
            }
            ++entries;
            bool const dot = key_length == 1 && key[0] == '.';
            bool const dotdot =
                key_length == 2 && key[0] == '.' && key[1] == '.';
            if (!dot && !dotdot) {
                ok = false;
            }
        }
        offset = le64(node_ + node::kRightLink);
        if (offset == static_cast<uint64_t>(kNullLink) ||
            offset == static_cast<uint64_t>(kFreeLink)) {
            break;
        }
    }
    return ok && entries == 2;
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