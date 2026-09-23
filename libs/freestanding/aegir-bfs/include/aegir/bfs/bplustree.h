/*
 * The Be File System's B+tree nodes.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A directory is an inode whose data stream begins with a tree header and
 * then holds nodes. This is the node half: reading the entries of one node,
 * and building a new node with one entry added or removed. It works on plain
 * bytes, so the same code serves the volume reader and the writer, and a host
 * test can call it without a volume at all.
 *
 * The layout is bplustree_node's: the fixed part, the keys packed in order,
 * then an array of uint16 key lengths at an off_t boundary, then an array of
 * off_t values. A node is a leaf exactly when its overflow link is
 * BPLUSTREE_NULL. Keys are compared bytewise, a shorter prefix first -- which
 * is what "a string" means to BFS (specs/bfs.md).
 */

#ifndef AEGIR_BFS_BPLUSTREE_H
#define AEGIR_BFS_BPLUSTREE_H

#include <aegir/bfs/layout.h>
#include <stdint.h>

namespace aegir::bfs {

/** The B+tree's header, as it sits at stream offset 0. */
struct TreeHeader {
    uint32_t node_size;
    uint32_t max_levels;
    uint32_t data_type;
    uint64_t root;
    uint64_t free_node;
    uint64_t maximum;
};

bool tree_header_parse(uint8_t const *bytes, TreeHeader *out) noexcept;
void tree_header_build(uint8_t *bytes, TreeHeader const &header) noexcept;

/** The order two keys sort in: negative, zero, positive. */
int key_compare(char const *a, uint32_t a_length, char const *b,
                uint32_t b_length) noexcept;

/** The order two keys sort in under a tree's `data_type` (aegir/bfs/layout.h's
 *  `kTree*Type`): a string is bytewise, the numeric types decode and compare
 *  as numbers, so an int64 index holds its keys in numeric order as Haiku's
 *  does. A key whose length does not match its type compares as bytes. */
int key_compare_typed(uint32_t data_type, uint8_t const *a, uint32_t a_length,
                      uint8_t const *b, uint32_t b_length) noexcept;

/** A node's fixed facts. `used` is the bytes the entries occupy. */
struct NodeInfo {
    uint16_t count;
    uint32_t all_key_length;
    int64_t overflow;
    uint32_t used;
};

bool node_info(uint8_t const *node, uint32_t node_size, NodeInfo *out) noexcept;

/** The value under `name`, or false when the node does not hold it. */
bool node_find(uint8_t const *node, uint32_t node_size, uint32_t data_type,
               char const *name, uint32_t length, uint64_t *value) noexcept;

/** The `index`th entry in key order, or false past the last. */
bool node_entry(uint8_t const *node, uint32_t node_size, uint16_t index,
                char *name, uint32_t *name_length, uint64_t *value) noexcept;

/** Insert or replace `name` in a rebuilt node, writing the result to `work`.
 *  `work` must be a buffer of `node_size` bytes distinct from `node`. False
 *  when the entry would not fit, or the name is not one BFS allows. `existed`
 *  reports whether the name was already there. */
bool node_insert(uint8_t *work, uint32_t node_size, uint32_t data_type,
                 uint8_t const *node, char const *name, uint32_t length,
                 uint64_t value, bool *existed) noexcept;

/** Remove `name` from a rebuilt node, writing the result to `work`. `work`
 *  must be a buffer of `node_size` bytes distinct from `node`. False when an
 *  entry in the node is malformed; `removed` reports whether the name was
 *  there. */
bool node_remove(uint8_t *work, uint32_t node_size, uint32_t data_type,
                 uint8_t const *node, char const *name, uint32_t length,
                 bool *removed) noexcept;

/** Replace the value under `index` in a rebuilt node, writing the result to
 *  `work`. Used to point a key's value at a duplicate array, or back. False
 *  when `index` is past the last entry. */
bool node_set_value(uint8_t *work, uint32_t node_size, uint8_t const *node,
                    uint16_t index, uint64_t value) noexcept;

/* A duplicate array node (specs/bfs.md): a whole node holding
 *  `{int64 count; off_t values[]}` at its overflow link, reached by a
 *  MakeLink(kDuplicateNode, offset) value. Aegir uses only these, never the
 *  fragment form Haiku also has; Haiku reads and appends to them either way. */

/** How many values a duplicate node of `node_size` may hold. */
uint32_t duplicate_capacity(uint32_t node_size) noexcept;

/** The values a duplicate node holds (a corrupt count is clamped). */
uint32_t duplicate_count(uint8_t const *node, uint32_t node_size) noexcept;

bool duplicate_value(uint8_t const *node, uint32_t node_size, uint32_t index,
                     uint64_t *out) noexcept;

/** Build a duplicate node with `count` values and its sibling links, which a
 *  chain of full nodes uses. */
void duplicate_build(uint8_t *node, uint32_t node_size, uint64_t left,
                     uint64_t right, uint64_t const *values,
                     uint32_t count) noexcept;

}  // namespace aegir::bfs

#endif  // AEGIR_BFS_BPLUSTREE_H