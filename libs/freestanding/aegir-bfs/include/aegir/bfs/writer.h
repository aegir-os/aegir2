/*
 * Writing a Be File System volume.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The write half of specs/bfs.md: making and removing files and directories,
 * writing a file's data into its extent tree, cutting a file down (or growing
 * it), and reading a directory's B+tree back to change it. It owns a block
 * allocator and all the scratch buffers the operations need, so the service
 * above it holds no format state at all.
 *
 * A directory's entries live in one leaf node here. A directory that outgrows
 * its node -- tens of entries -- is refused rather than miswritten; the node
 * split and the internal cursor are the next step in this phase. The same is
 * true of a stream that outgrows the indirect runs: a run is added to the
 * direct runs, then to the indirect array, and a stream that needs the double
 * indirect is refused, not corrupted.
 */

#ifndef AEGIR_BFS_WRITER_H
#define AEGIR_BFS_WRITER_H

#include <aegir/bfs/allocator.h>
#include <aegir/bfs/journal.h>
#include <aegir/bfs/layout.h>
#include <aegir/bfs/volume.h>
#include <stdint.h>

namespace aegir::bfs {

class Writer {
public:
    /** Bind to a writable, valid volume. */
    bool open(Volume *volume) noexcept;

    /** Make a file (`mode` a regular file's) or a directory under the
     *  directory inode at `parent_block`, named `name`, stamped `time`. A
     *  directory is born with its dot and dotdot. The new inode's block is
     *  `*out_block`. False on a full volume, a bad name, or a parent whose
     *  tree is full. */
    bool create(uint64_t parent_block, char const *name, uint32_t name_length,
                uint32_t mode, int64_t time, uint64_t *out_block) noexcept;

    /** Remove the entry `name` from the directory at `parent_block` and free
     *  what it named -- its data runs and its inode. A directory that is not
     *  empty refuses. */
    bool remove(uint64_t parent_block, char const *name,
                uint32_t name_length) noexcept;

    /** Rename `from` to `to`, both in the directory at `parent_block`. The
     *  inode and its data do not move; the destination must not exist. */
    bool rename(uint64_t parent_block, char const *from, uint32_t from_length,
                char const *to, uint32_t to_length) noexcept;

    /** Write `length` bytes at logical `offset` in the stream of the inode at
     *  `inode_block`, growing the stream with zeroed blocks as needed. */
    bool write(uint64_t inode_block, uint64_t offset, uint8_t const *bytes,
               uint32_t length, int64_t time) noexcept;

    /** Resize the inode's stream to `size`: free the tail it no longer needs,
     *  or grow it with zeroed blocks. */
    bool truncate(uint64_t inode_block, uint64_t size, int64_t time) noexcept;

    /** Free the inode at `block`: its stream's runs, then the block itself.
     *  The directory entry has to be gone already. */
    bool destroy(uint64_t block) noexcept;

    /** Write `length` bytes of the attribute `name` at `offset`, making it
     *  with `type` when it is not there. A value too large for the inode's
     *  small_data section gets an attribute inode under the attribute
     *  directory. Writing an existing attribute with a different type is
     *  refused. `*written` is the count. */
    bool attr_write(uint64_t inode_block, char const *name, uint32_t name_length,
                    uint32_t type, uint64_t offset, uint8_t const *bytes,
                    uint32_t length, uint32_t *written, int64_t time) noexcept;

    /** Remove the attribute `name` -- from the inode or from the attribute
     *  directory and its inode. False when it is not there. */
    bool attr_remove(uint64_t inode_block, char const *name,
                     uint32_t name_length) noexcept;

    /** Add `value` under `key` in the index inode at `index_block`, whose tree
     *  keeps its keys by their type. A key that is not there takes `value`; a
     *  key already there gets `value` added to its duplicate array, so two
     *  inodes that share a size or a name are both found. */
    bool index_insert(uint64_t index_block, uint8_t const *key,
                      uint32_t key_length, uint64_t value) noexcept;

    /** Remove `value` from under `key` in the index at `index_block`. The key
     *  goes when its last value does. False on a corrupt index; `removed`
     *  reports whether the value was there. */
    bool index_remove(uint64_t index_block, uint8_t const *key,
                      uint32_t key_length, uint64_t value,
                      bool *removed) noexcept;

private:
    /* Each public operation is one journal transaction (specs/bfs.md): the
     * metadata it changes is buffered and written to the log before any block
     * goes home, so a crash leaves either nothing or a log replay can repair.
     * The `_blocks` forms are the operation without the transaction, for an
     * operation that already holds one (an attribute write into its inode, or
     * a removal's destroy). */
    bool finish(bool ok) noexcept;
    bool create_blocks(uint64_t parent_block, char const *name,
                       uint32_t name_length, uint32_t mode, int64_t time,
                       uint64_t *out_block) noexcept;
    bool remove_blocks(uint64_t parent_block, char const *name,
                       uint32_t name_length) noexcept;
    bool rename_blocks(uint64_t parent_block, char const *from,
                       uint32_t from_length, char const *to,
                       uint32_t to_length) noexcept;
    bool write_blocks(uint64_t inode_block, uint64_t offset, uint8_t const *bytes,
                      uint32_t length, int64_t time) noexcept;
    bool truncate_blocks(uint64_t inode_block, uint64_t size, int64_t time) noexcept;
    bool attr_write_blocks(uint64_t inode_block, char const *name,
                           uint32_t name_length, uint32_t type, uint64_t offset,
                           uint8_t const *bytes, uint32_t length,
                           uint32_t *written, int64_t time) noexcept;
    bool attr_remove_blocks(uint64_t inode_block, char const *name,
                            uint32_t name_length) noexcept;

    /* The attribute work without its index maintenance; the `_blocks` forms
     * call these and then keep the BEOS:APP_SIG index in step. */
    bool attr_write_impl(uint64_t inode_block, char const *name,
                         uint32_t name_length, uint32_t type, uint64_t offset,
                         uint8_t const *bytes, uint32_t length,
                         uint32_t *written, int64_t time) noexcept;
    bool attr_remove_impl(uint64_t inode_block, char const *name,
                          uint32_t name_length) noexcept;

    /* The first bytes of the attribute `name` on the inode at `inode_block`,
     * for its index key; false when the attribute is not there. */
    bool attribute_key(uint64_t inode_block, char const *name,
                       uint32_t name_length, uint8_t *out,
                       uint32_t *length) noexcept;

    /* Where a node split left things: the node at `offset` became `left`, a
     * fresh node `right` holds the greater half, and `separator` (the
     * greatest key of the left) goes into the parent. */
    struct TreeSplit {
        bool split;
        char separator[kMaxName];
        uint32_t separator_length;
        uint64_t left;
        uint64_t right;
    };

    bool read_inode_block(uint64_t block, uint8_t *out) noexcept;
    /* The order two keys sort in under the open tree's key type. */
    int key_order(char const *a, uint32_t a_length, uint8_t const *b,
                  uint32_t b_length) const noexcept;
    bool tree_header(uint64_t parent_block, uint8_t *stream, uint32_t *node_size,
                     uint64_t *root, uint64_t *maximum) noexcept;
    bool tree_edit(uint64_t parent_block, char const *name, uint32_t name_length,
                   uint64_t value, bool insert, bool *existed) noexcept;
    bool dir_is_empty(uint64_t dir_block) noexcept;

    /* Borrow a node's entries into the gather arrays below. */
    bool gather(uint8_t const *node, uint32_t node_size, uint16_t *count_out,
                int64_t *overflow_out) noexcept;
    bool node_read(uint64_t offset, uint32_t node_size, uint8_t *out) noexcept;
    bool node_write(uint64_t offset, uint32_t node_size, uint8_t const *node) noexcept;
    bool header_write() noexcept;
    bool append_node(uint64_t parent_block, uint32_t node_size,
                     uint64_t *out_offset) noexcept;
    bool insert_into(uint64_t offset, uint32_t node_size, char const *name,
                     uint32_t name_length, uint64_t value, TreeSplit *out) noexcept;
    bool remove_into(uint64_t offset, uint32_t node_size, char const *name,
                     uint32_t name_length, bool *removed) noexcept;
    bool split_leaf(uint8_t const *node, uint32_t node_size, uint64_t offset,
                    char const *name, uint32_t name_length, uint64_t value,
                    TreeSplit *out) noexcept;
    bool split_internal(uint8_t const *node, uint32_t node_size, uint64_t offset,
                        char const *name, uint32_t name_length, uint64_t value,
                        uint64_t replace_with, TreeSplit *out) noexcept;
    bool write_new_root(uint64_t root_block, TreeSplit const &split,
                        uint32_t node_size) noexcept;

    bool append_run(uint8_t *stream, Run const &run, uint32_t *rest) noexcept;
    bool append_double(uint8_t *stream, Run run) noexcept;
    uint32_t double_indirect_blocks() const noexcept;
    uint64_t stream_blocks(uint8_t const *stream) const noexcept;
    bool grow_to(uint8_t *stream, uint64_t needed_blocks,
                 uint64_t *covered) noexcept;
    bool zero_range(uint8_t *stream, uint64_t from, uint64_t to) noexcept;
    bool free_stream(uint8_t const *stream) noexcept;
    bool free_double(uint8_t *stream) noexcept;
    bool trim_stream(uint8_t *stream, uint64_t new_blocks) noexcept;

    /* Attributes: the inode's own small_data section, or an inode under its
     * attribute directory (specs/bfs.md). */
    bool attr_dir(uint64_t inode_block, int64_t time, uint64_t *dir_block) noexcept;
    bool attr_inode_create(uint64_t dir_block, char const *name,
                           uint32_t name_length, uint32_t type, int64_t time,
                           uint64_t *attr_block) noexcept;

    /* An index is a stream holding a tree whose keys are compared by the
     * header's data_type, and whose a leaf value may point at a duplicate node
     * (specs/bfs.md). The `_blocks` forms are the operation inside a larger
     * transaction; stage 4 calls them beside the operation that changed a
     * name, a size or a time. */
    bool index_insert_blocks(uint64_t index_block, uint8_t const *key,
                             uint32_t key_length, uint64_t value) noexcept;
    bool index_remove_blocks(uint64_t index_block, uint8_t const *key,
                             uint32_t key_length, uint64_t value,
                             bool *removed) noexcept;

    /* Descend to the leaf holding `key`: whether it is there, the leaf's
     * offset, the entry's index in it, and the value (possibly a duplicate
     * link). */
    bool index_descend(uint64_t offset, uint32_t node_size, uint8_t const *key,
                       uint32_t key_length, bool *found, uint64_t *leaf,
                       uint16_t *index, uint64_t *value) noexcept;

    /* Add a value to the entry at `leaf`/`index` whose present value is
     * `old_value`, making or extending its duplicate array. */
    bool index_add_value(uint64_t leaf, uint16_t index, uint64_t old_value,
                         uint64_t value, uint32_t node_size) noexcept;

    /* Remove a value from that entry, collapsing the duplicate array to a
     * plain value when one is left. */
    bool index_drop_value(uint64_t leaf, uint16_t index, uint64_t old_value,
                          uint8_t const *key, uint32_t key_length,
                          uint64_t value, uint32_t node_size,
                          bool *removed) noexcept;

    /* Put a tree node back on the header's free list, as Haiku's CachedNode
     * does, so a later Haiku mount reuses it. */
    bool free_tree_node(uint64_t offset, uint32_t node_size) noexcept;

    /* The name, size and last_modified indices follow an inode as it is made,
     * renamed, resized and removed (specs/bfs.md). Each `index_add`/`index_drop`
     * is a no-op on a volume with no such index. */
    bool index_add(char const *index, uint32_t index_length, uint8_t const *key,
                   uint32_t key_length, uint64_t value) noexcept;
    bool index_drop(char const *index, uint32_t index_length, uint8_t const *key,
                    uint32_t key_length, uint64_t value) noexcept;
    bool index_on_create(uint64_t block, char const *name, uint32_t name_length,
                         uint32_t mode, int64_t size, int64_t time) noexcept;
    bool index_on_remove(uint64_t block, char const *name, uint32_t name_length,
                         uint32_t mode, int64_t size, int64_t time) noexcept;
    bool index_on_rename(uint64_t block, char const *from, uint32_t from_length,
                         char const *to, uint32_t to_length) noexcept;
    bool index_on_resize(uint64_t block, uint32_t mode, int64_t old_size,
                         int64_t new_size) noexcept;
    bool index_on_time(uint64_t block, uint32_t mode, int64_t old_time,
                       int64_t new_time) noexcept;

    Volume *volume_ = nullptr;
    Allocator allocator_;
    Journal journal_;
    uint64_t edit_parent_ = 0; /* the directory inode a tree edit is growing */
    uint32_t tree_type_ = kTreeStringType; /* the open tree's key type */
    /* An attribute index keys on the attribute's value, which Haiku caps at
     * MAX_INDEX_KEY_LENGTH (255) bytes. */
    static constexpr uint32_t kMaxIndexKey = 255;
    uint8_t index_old_key_[kMaxIndexKey] = {};
    uint8_t index_new_key_[kMaxIndexKey] = {};

    uint8_t inode_[kMaxBlockSize] = {};
    uint8_t stream_[data::kBytes] = {};
    uint8_t hdr_[tree_header::kBytes] = {};
    mutable uint8_t node_[kMaxBlockSize] = {};
    uint8_t parent_[kMaxBlockSize] = {};
    uint8_t split_src_[kMaxBlockSize] = {};
    mutable uint8_t work_[kMaxBlockSize] = {};
    uint8_t fresh_[kMaxBlockSize] = {};
    uint8_t zero_[kMaxBlockSize] = {};
    uint8_t attr_[kMaxBlockSize] = {};

    /* A node holds far fewer than this many entries; the arrays are members,
     * not locals, because a split runs on a service stack. */
    static constexpr uint32_t kMaxNodeEntries = 200;
    uint8_t const *g_keys_[kMaxNodeEntries] = {};
    uint16_t g_lengths_[kMaxNodeEntries] = {};
    uint64_t g_values_[kMaxNodeEntries] = {};
    uint8_t const *c_keys_[kMaxNodeEntries] = {};
    uint16_t c_lengths_[kMaxNodeEntries] = {};
    uint64_t c_values_[kMaxNodeEntries] = {};
};

}  // namespace aegir::bfs

#endif  // AEGIR_BFS_WRITER_H