#!/usr/bin/env python3
"""Build a Be File System (BFS) volume inside a disk image.

Copyright (c) 2026 Robert Roland
SPDX-License-Identifier: MIT

The test disk needs a BFS partition the filesystem service can mount, and the
service is the thing under test -- so the volume is built here, host-side,
without Haiku. The format is the one specs/bfs.md fixes and Haiku's on-disk
structures define: the superblock at offset 512, allocation-group block
bitmaps, the log region, inodes of one block each, and a directory's entries
in a B+tree stored in the directory inode's data stream.

The builder makes a small but valid little-endian BFS: a root directory with
a known file and a nested directory, the "." and ".." entries a regular
directory carries, each inode's file-name small data, and the four standard
indices (name, BEOS:APP_SIG, last_modified, size) with their entries, so a
mount trusts them as Haiku's do (specs/bfs.md's indices).

The public entry point is make_bfs(): give it the image buffer, the
partition's byte offset and size, a label, and a tree of entries.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

BLOCK = 2048
NODE = 1024  # the B+tree node size
SECTOR = 512

# BFS's magic numbers, as the C multicharacter constants pack them (big-endian
# into an int, then written little-endian), which is why the on-disk magic is
# not the ASCII string.
MAGIC1 = 0x42465331  # 'BFS1'
MAGIC2 = 0xDD121031
MAGIC3 = 0x15B6830E
BYTE_ORDER_LENDIAN = 0x42494745  # 'BIGE' -- a little-endian volume's marker
CLEAN = 0x434C454E  # 'CLEN'
DIRTY = 0x44495254  # 'DIRT'

INODE_MAGIC1 = 0x3BBE0AD9
INODE_IN_USE = 0x00000001

# POSIX modes and BFS's extended type bits (sys/stat.h).
S_IFDIR = 0o040000
S_IFREG = 0o100000
S_ATTR_DIR = 0o01000000000
S_ATTR = 0o02000000000
S_INDEX_DIR = 0o04000000000
S_STR_INDEX = 0o00100000000
S_LONG_LONG_INDEX = 0o00010000000

BPLUSTREE_MAGIC = 0x69F6C2E8
BPLUSTREE_STRING_TYPE = 0
BPLUSTREE_INT64_TYPE = 3

# The index types an index inode's `type` field carries: Be's type codes.
TYPE_CSTR = 0x43535452  # 'CSTR', B_STRING_TYPE
TYPE_LLNG = 0x4C4C4E47  # 'LLNG', B_INT64_TYPE

# A duplicate node a key's value points at holds at most this many values.
NUM_DUPLICATE_VALUES = 125

FILE_NAME_TYPE = 0x43535452  # 'CSTR'
FILE_NAME_NAME = 0x13

ZERO_RUN = struct.pack("<iHH", 0, 0, 0)


def _align8(value: int) -> int:
    return (value + 7) & ~7


def _w16(buf: bytearray, at: int, value: int) -> None:
    struct.pack_into("<H", buf, at, value)


def _w32(buf: bytearray, at: int, value: int) -> None:
    struct.pack_into("<I", buf, at, value & 0xFFFFFFFF)


def _w64(buf: bytearray, at: int, value: int) -> None:
    struct.pack_into("<Q", buf, at, value & 0xFFFFFFFFFFFFFFFF)


def _run(block: int, length: int, ag_shift: int) -> bytes:
    ag = block >> ag_shift
    start = block & ((1 << ag_shift) - 1)
    return struct.pack("<iHH", ag, start, length)


def _geometry(num_blocks: int) -> tuple[int, int, int]:
    """The allocation-group geometry Haiku's Initialize computes: the group
    shift, the bitmap blocks per group, and the group count."""
    bits_per_block = BLOCK * 8
    bitmap_blocks = (num_blocks + bits_per_block - 1) // bits_per_block
    group_shift = 13
    i = 8192
    while i < bits_per_block:
        group_shift += 1
        i *= 2
    bitmap_per_group = 1
    while True:
        num_groups = (bitmap_blocks + bitmap_per_group - 1) // bitmap_per_group
        if num_groups > 56 and group_shift < 16:
            group_shift += 1
            bitmap_per_group *= 2
        else:
            break
    return group_shift, bitmap_per_group, num_groups


def _small_data(*, type_code: int, name: bytes, data: bytes) -> bytes:
    entry = bytearray(8)
    _w32(entry, 0, type_code)
    _w16(entry, 4, len(name))
    _w16(entry, 6, len(data))
    entry += name
    entry += b"\x00" * 3
    entry += data
    entry += b"\x00"
    return bytes(entry)


def _inode(*, run: bytes, mode: int, parent: bytes, attributes: bytes,
           size: int, runs: list[bytes], name: bytes | None, time: int,
           type_code: int = 0) -> bytes:
    inode = bytearray(BLOCK)
    _w32(inode, 0, INODE_MAGIC1)
    inode[4:12] = run
    _w32(inode, 12, 0)  # uid
    _w32(inode, 16, 0)  # gid
    _w32(inode, 20, mode)
    _w32(inode, 24, INODE_IN_USE)
    _w64(inode, 28, time)  # create_time
    _w64(inode, 36, time)  # last_modified_time
    inode[44:52] = parent
    inode[52:60] = attributes
    _w32(inode, 60, type_code)  # an attribute/index's type_code, else zero
    _w32(inode, 64, BLOCK)  # inode_size
    _w32(inode, 68, 0)  # etc

    # data_stream at 72: direct[12], max_direct_range, indirect,
    # max_indirect_range, double_indirect, max_double_indirect_range, size.
    for index in range(12):
        inode[72 + index * 8 : 72 + index * 8 + 8] = runs[index] if index < len(runs) else ZERO_RUN
    covered = 0
    for r in runs:
        covered += struct.unpack("<H", r[6:8])[0] * BLOCK
    if runs:
        _w64(inode, 168, covered)  # max_direct_range
    inode[176:184] = ZERO_RUN  # indirect
    inode[192:200] = ZERO_RUN  # double_indirect
    _w64(inode, 208, size)

    _w64(inode, 216, time)  # status_change_time

    if name is not None:
        entry = _small_data(type_code=FILE_NAME_TYPE, name=bytes([FILE_NAME_NAME]),
                            data=name)
        inode[232 : 232 + len(entry)] = entry
    return bytes(inode)


def _tree(entries: list[tuple[bytes, int]]) -> bytes:
    """A directory's B+tree: a header at stream offset 0 and a leaf root node
    at offset NODE. The entries are (name, inode block), already sorted by
    BFS's string comparison (bytewise, shorter-prefix first)."""
    keys = [name for name, _ in entries]
    values = [value for _, value in entries]
    all_key_length = sum(len(key) for key in keys)
    used = _align8(28 + all_key_length) + len(keys) * (2 + 8)
    if used > NODE:
        raise ValueError("directory does not fit one B+tree node")

    tree = bytearray(NODE * 2)
    _w32(tree, 0, BPLUSTREE_MAGIC)
    _w32(tree, 4, NODE)
    _w32(tree, 8, 1)  # max_number_of_levels
    _w32(tree, 12, BPLUSTREE_STRING_TYPE)
    _w64(tree, 16, NODE)  # root_node_pointer
    _w64(tree, 24, 0xFFFFFFFFFFFFFFFF)  # free_node_pointer: none
    _w64(tree, 32, NODE * 2)  # maximum_size

    base = NODE
    _w64(tree, base, 0xFFFFFFFFFFFFFFFF)  # left_link
    _w64(tree, base + 8, 0xFFFFFFFFFFFFFFFF)  # right_link
    _w64(tree, base + 16, 0xFFFFFFFFFFFFFFFF)  # overflow_link -> a leaf
    _w16(tree, base + 24, len(keys))
    _w16(tree, base + 26, all_key_length)

    at = base + 28
    for key in keys:
        tree[at : at + len(key)] = key
        at += len(key)
    at = base + _align8(28 + all_key_length)
    # The key-length array holds cumulative offsets, not individual lengths:
    # Haiku's _InsertKey adds and KeyAt subtracts (BPlusTree.cpp).
    cumulative = 0
    for key in keys:
        cumulative += len(key)
        _w16(tree, at, cumulative)
        at += 2
    for value in values:
        _w64(tree, at, value)
        at += 8
    return bytes(tree)


def _sort_key(entry: tuple[bytes, int]) -> bytes:
    return entry[0]


def _make_link(link_type: int, offset: int) -> int:
    return ((link_type << 62) | (offset & 0x3FFFFFFFFFFFFC00)) & 0xFFFFFFFFFFFFFFFF


def _typed_tree(data_type: int, entries: list[tuple[bytes, int]]) -> bytes:
    """An index's B+tree: one leaf, plus a duplicate node for each repeated
    key, so `last_modified` with several inodes at the same time is held the
    way Haiku holds it. Keys sort by their type, not by their bytes, so an
    int64 key is in numeric order. A tree bigger than one leaf is refused, as
    the directory builder refuses one."""
    if data_type == BPLUSTREE_INT64_TYPE:
        ordered = sorted(entries, key=lambda e: struct.unpack("<q", e[0])[0])
    else:
        ordered = sorted(entries, key=lambda e: e[0])

    groups: list[list] = []  # [key, [values...]]
    for key, value in ordered:
        if groups and groups[-1][0] == key:
            groups[-1][1].append(value)
        else:
            groups.append([key, [value]])

    duplicate_nodes: list[tuple[int, list[int]]] = []
    offset = NODE * 2
    keys: list[bytes] = []
    values: list[int] = []
    for key, group in groups:
        group.sort()
        keys.append(key)
        if len(group) == 1:
            values.append(group[0])
        else:
            if len(group) > NUM_DUPLICATE_VALUES:
                raise ValueError("index duplicate array too long for one node")
            duplicate_nodes.append((offset, group))
            values.append(_make_link(2, offset))  # 2: a duplicate node
            offset += NODE

    total = NODE * (2 + len(duplicate_nodes))
    tree = bytearray(total)
    _w32(tree, 0, BPLUSTREE_MAGIC)
    _w32(tree, 4, NODE)
    _w32(tree, 8, 1)  # max_number_of_levels
    _w32(tree, 12, data_type)
    _w64(tree, 16, NODE)  # root_node_pointer
    _w64(tree, 24, 0xFFFFFFFFFFFFFFFF)  # free_node_pointer: none
    _w64(tree, 32, total)  # maximum_size

    all_key_length = sum(len(key) for key in keys)
    if _align8(28 + all_key_length) + len(keys) * (2 + 8) > NODE:
        raise ValueError("index does not fit one B+tree node")

    base = NODE
    _w64(tree, base, 0xFFFFFFFFFFFFFFFF)  # left_link
    _w64(tree, base + 8, 0xFFFFFFFFFFFFFFFF)  # right_link
    _w64(tree, base + 16, 0xFFFFFFFFFFFFFFFF)  # overflow_link -> a leaf
    _w16(tree, base + 24, len(keys))
    _w16(tree, base + 26, all_key_length)
    at = base + 28
    for key in keys:
        tree[at : at + len(key)] = key
        at += len(key)
    at = base + _align8(28 + all_key_length)
    cumulative = 0
    for key in keys:
        cumulative += len(key)
        _w16(tree, at, cumulative)
        at += 2
    for value in values:
        _w64(tree, at, value)
        at += 8

    for duplicate_offset, group in duplicate_nodes:
        _w64(tree, duplicate_offset, 0xFFFFFFFFFFFFFFFF)  # left_link
        _w64(tree, duplicate_offset + 8, 0xFFFFFFFFFFFFFFFF)  # right_link
        _w64(tree, duplicate_offset + 16, len(group))  # count
        dat = duplicate_offset + 24
        for value in group:
            _w64(tree, dat, value)
            dat += 8
    return bytes(tree)


def make_bfs(buf: bytearray, offset: int, size: int, label: str,
             tree: list) -> None:
    """Write a BFS volume into `buf` at `offset`.

    `tree` is a list of entries, each one of:
        ("file", name: str, content: bytes)
        ("dir", name: str, subtree: list)
    """
    num_blocks = size // BLOCK
    ag_shift, blocks_per_ag, num_ags = _geometry(num_blocks)
    bitmap_blocks = num_ags * blocks_per_ag
    log_start = bitmap_blocks + 1
    if num_blocks <= 20480:
        log_size = 512
    elif size > 1 << 30:
        log_size = 4096
    else:
        log_size = 2048
    data_start = log_start + log_size

    next_block = data_start

    def take(count: int = 1) -> int:
        nonlocal next_block
        block = next_block
        next_block += count
        return block

    # The layout: every inode and every directory tree is a block of its own,
    # and a file's data follows. Built parents-first would need the children's
    # inode numbers before they exist, so directories are built in a second
    # pass, once each child has a block.
    inodes: list[tuple[int, bytes]] = []
    data_blocks: list[tuple[int, bytes]] = []

    # The standard indices' entries, gathered as the tree is built: the name
    # index takes every named inode, the size and last_modified indices the
    # files (specs/bfs.md).
    name_index: list[tuple[bytes, int]] = []
    size_index: list[tuple[bytes, int]] = []
    mtime_index: list[tuple[bytes, int]] = []

    def build(entries: list, own_block: int, parent_block: int,
              own_name: str | None = None) -> None:
        # A directory owns a tree block, and every child owns an inode (and,
        # for a file, its data). The children are taken first so the parent's
        # tree can name their inode numbers; this directory's own inode is
        # written last, once its tree block is known.
        tree_block = take()
        table: list[tuple[bytes, int]] = []

        for entry in entries:
            kind, name = entry[0], entry[1]
            child_block = take()
            if kind == "file":
                content = entry[2]
                runs: list[bytes] = []
                if content:
                    blocks = (len(content) + BLOCK - 1) // BLOCK
                    for b in range(blocks):
                        data_block = take()
                        runs.append(_run(data_block, 1, ag_shift))
                        data_blocks.append((data_block, content[b * BLOCK : (b + 1) * BLOCK]))
                inodes.append((child_block, _inode(
                    run=_run(child_block, 1, ag_shift), mode=S_IFREG | 0o644,
                    parent=_run(own_block, 1, ag_shift), attributes=ZERO_RUN,
                    size=len(content), runs=runs, name=name.encode("utf-8"), time=0)))
                table.append((name.encode("utf-8"), child_block))
                name_index.append((name.encode("utf-8"), child_block))
                size_index.append((struct.pack("<q", len(content)), child_block))
                mtime_index.append((struct.pack("<q", 0), child_block))
            else:  # dir
                table.append((name.encode("utf-8"), child_block))
                name_index.append((name.encode("utf-8"), child_block))
                build(entry[2], child_block, own_block, name)

        table.append((b".", own_block))
        table.append((b"..", parent_block))
        table.sort(key=_sort_key)
        data_blocks.append((tree_block, _tree(table)))
        inodes.append((own_block, _inode(
            run=_run(own_block, 1, ag_shift), mode=S_IFDIR | S_STR_INDEX | 0o755,
            parent=_run(parent_block, 1, ag_shift), attributes=ZERO_RUN,
            size=NODE * 2, runs=[_run(tree_block, 1, ag_shift)],
            name=own_name.encode("utf-8") if own_name is not None else None,
            time=0)))

    root_block = take()
    build(tree, root_block, root_block)

    # The four standard indices (specs/bfs.md): an indices root whose tree
    # names them, and each index a tree over the inode blocks. An index inode
    # has S_INDEX_DIR and S_<type>_INDEX, carries the Be type code, and lives
    # under the indices root; it has no file name of its own.
    def place_tree(tree_bytes: bytes) -> bytes:
        blocks = (len(tree_bytes) + BLOCK - 1) // BLOCK
        base = take(blocks)
        for i in range(blocks):
            piece = bytes(tree_bytes[i * BLOCK : (i + 1) * BLOCK])
            data_blocks.append((base + i, piece.ljust(BLOCK, b"\x00")))
        return _run(base, blocks, ag_shift)

    name_block = take()
    name_tree = _typed_tree(BPLUSTREE_STRING_TYPE, name_index)
    name_run = place_tree(name_tree)

    appsig_block = take()
    appsig_tree = _typed_tree(BPLUSTREE_STRING_TYPE, [])
    appsig_run = place_tree(appsig_tree)

    mtime_block = take()
    mtime_tree = _typed_tree(BPLUSTREE_INT64_TYPE, mtime_index)
    mtime_run = place_tree(mtime_tree)

    size_block = take()
    size_tree = _typed_tree(BPLUSTREE_INT64_TYPE, size_index)
    size_run = place_tree(size_tree)

    indices_root_block = take()
    indices_root_run = _run(indices_root_block, 1, ag_shift)
    indices_tree = _typed_tree(BPLUSTREE_STRING_TYPE, [
        (b"BEOS:APP_SIG", appsig_block),
        (b"last_modified", mtime_block),
        (b"name", name_block),
        (b"size", size_block),
    ])
    indices_tree_run = place_tree(indices_tree)

    inodes.append((indices_root_block, _inode(
        run=indices_root_run, mode=S_INDEX_DIR | S_STR_INDEX | S_IFDIR | 0o700,
        parent=indices_root_run, attributes=ZERO_RUN, size=len(indices_tree),
        runs=[indices_tree_run], name=None, time=0)))

    std_str = S_INDEX_DIR | S_IFDIR | S_STR_INDEX
    std_i64 = S_INDEX_DIR | S_IFDIR | S_LONG_LONG_INDEX
    for block, tree_bytes, tree_run, mode, type_code in (
            (name_block, name_tree, name_run, std_str, TYPE_CSTR),
            (appsig_block, appsig_tree, appsig_run, std_str, TYPE_CSTR),
            (mtime_block, mtime_tree, mtime_run, std_i64, TYPE_LLNG),
            (size_block, size_tree, size_run, std_i64, TYPE_LLNG)):
        inodes.append((block, _inode(
            run=_run(block, 1, ag_shift), mode=mode, parent=indices_root_run,
            attributes=ZERO_RUN, size=len(tree_bytes), runs=[tree_run],
            name=None, time=0, type_code=type_code)))

    used_blocks = next_block

    for block, content in inodes + data_blocks:
        start = offset + block * BLOCK
        buf[start : start + len(content)] = content

    # The block bitmap: every block below data_start is reserved, and every
    # allocated block is marked. Bit i of a little-endian 32-bit chunk is
    # block i.
    bitmap = bytearray(bitmap_blocks * BLOCK)
    for block in range(used_blocks):
        bitmap[(block // 32) * 4 + (block % 32) // 8] |= 1 << (block % 8)
    start = offset + 1 * BLOCK
    buf[start : start + len(bitmap)] = bitmap

    # The superblock, at offset 512 of the partition.
    superblock = bytearray(SECTOR)
    encoded = label.encode("utf-8")[:31]
    superblock[0 : len(encoded)] = encoded
    _w32(superblock, 32, MAGIC1)
    _w32(superblock, 36, BYTE_ORDER_LENDIAN)
    _w32(superblock, 40, BLOCK)
    _w32(superblock, 44, BLOCK.bit_length() - 1)
    _w64(superblock, 48, num_blocks)
    _w64(superblock, 56, used_blocks)
    _w32(superblock, 64, BLOCK)  # inode_size
    _w32(superblock, 68, MAGIC2)
    _w32(superblock, 72, blocks_per_ag)
    _w32(superblock, 76, ag_shift)
    _w32(superblock, 80, num_ags)
    _w32(superblock, 84, CLEAN)
    superblock[88:96] = _run(log_start, log_size, ag_shift)
    _w64(superblock, 96, log_start)  # log_start
    _w64(superblock, 104, log_start)  # log_end: clean
    _w32(superblock, 112, MAGIC3)
    superblock[116:124] = _run(root_block, 1, ag_shift)
    superblock[124:132] = _run(indices_root_block, 1, ag_shift)
    start = offset + 512
    buf[start : start + SECTOR] = superblock

    # The boot block and the old ext2 superblock location, erased as mkfs
    # does, so a leftover boot sector cannot confuse identification.
    buf[offset : offset + SECTOR] = b"\x00" * SECTOR
    buf[offset + 1024 : offset + 1536] = b"\x00" * SECTOR


def _main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("offset", type=lambda v: int(v, 0))
    parser.add_argument("size", type=lambda v: int(v, 0))
    parser.add_argument("--label", default="BFS")
    args = parser.parse_args()

    with args.image.open("r+b") as handle:
        handle.seek(0, 2)
        if handle.tell() < args.offset + args.size:
            handle.truncate(args.offset + args.size)
        handle.seek(0)
        buf = bytearray(handle.read())
        tree = [
            ("file", "HELLO.TXT", b"a Be File System file, read off a disk Aegir built itself\n"),
            ("dir", "SUBDIR", [
                ("file", "NESTED.TXT", b"two components deep, through a Be File System directory\n"),
            ]),
        ]
        make_bfs(buf, args.offset, args.size, args.label, tree)
        handle.seek(0)
        handle.write(buf)
    print(f"mkfs_bfs: {args.image}: BFS at {args.offset}, {args.size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
