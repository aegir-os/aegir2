/*
 * The Be File System's on-disk structures, and nothing else.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * These are the bytes specs/bfs.md fixes and Haiku's bfs.h defines: the
 * superblock, an inode, a block run, a data stream, a B+tree header and node.
 * Everything is little-endian, and every reader here is a byte reader -- the
 * structures are packed on disk, so nothing is read by casting a pointer to a
 * struct. The offsets are the manual's, written down so a mistake is a line
 * number rather than a mysterious field.
 */

#ifndef AEGIR_BFS_LAYOUT_H
#define AEGIR_BFS_LAYOUT_H

#include <stdint.h>

namespace aegir::bfs {

/** The largest block size the format allows (block_size is 1 << block_shift,
 *  and the shift is at most 13 in Haiku's Initialize's choices). */
constexpr uint32_t kMaxBlockSize = 8192;
constexpr uint32_t kSuperblockOffset = 512;
constexpr uint32_t kSuperblockBytes = 512;

/** The magic numbers, as C multicharacter constants pack them. */
constexpr uint32_t kMagic1 = 0x42465331; /* 'BFS1' */
constexpr uint32_t kMagic2 = 0xdd121031;
constexpr uint32_t kMagic3 = 0x15b6830e;
constexpr uint32_t kByteOrderLittleEndian = 0x42494745; /* 'BIGE' */
constexpr uint32_t kClean = 0x434c454e;                 /* 'CLEN' */
constexpr uint32_t kDirty = 0x44495254;                 /* 'DIRT' */

/** The inode's magic, and the flags a live one carries. */
constexpr uint32_t kInodeMagic1 = 0x3bbe0ad9;
constexpr uint32_t kInodeInUse = 0x00000001;
constexpr uint32_t kInodeAttrInode = 0x00000004;
constexpr uint32_t kInodeDeleted = 0x00000010;

/** The POSIX type bits (sys/stat.h), and BFS's extended type bits. */
constexpr uint32_t kModeTypeMask = 0xf000;
constexpr uint32_t kModeDirectory = 0x4000;     /* S_IFDIR */
constexpr uint32_t kModeRegular = 0x8000;       /* S_IFREG */
constexpr uint32_t kModeAttrDir = 0x08000000;   /* S_ATTR_DIR */
constexpr uint32_t kModeAttr = 0x10000000;      /* S_ATTR */
constexpr uint32_t kModeIndexDir = 0x20000000;  /* S_INDEX_DIR */
constexpr uint32_t kModeStrIndex = 0x01000000;  /* S_STR_INDEX */

/** The B+tree's magic. */
constexpr uint32_t kTreeMagic = 0x69f6c2e8;
constexpr uint32_t kTreeNodeSize = 1024; /* BPLUSTREE_NODE_SIZE, hard-coded */
constexpr uint32_t kNumArrayBlocks = 4;  /* NUM_ARRAY_BLOCKS */
constexpr uint32_t kDoubleIndirectArraySize = 4096; /* DOUBLE_INDIRECT_ARRAY_SIZE */

/** Roles a key type names (bplustree data_type). Directories are string. */
constexpr uint32_t kTreeStringType = 0;
constexpr uint32_t kTreeInt32Type = 1;
constexpr uint32_t kTreeUInt32Type = 2;
constexpr uint32_t kTreeInt64Type = 3;
constexpr uint32_t kTreeUInt64Type = 4;
constexpr uint32_t kTreeFloatType = 5;
constexpr uint32_t kTreeDoubleType = 6;

/** A tree value can point at a duplicate array instead of naming an inode:
 *  Haiku's `bplustree_node::MakeLink` packs a type in the top two bits and a
 *  node offset in the rest. A duplicate node is a whole node holding
 *  `{int64 count; off_t values[]}`; Aegir uses only those, never the fragment
 *  nodes Haiku also knows, because a duplicate node is the simple case Haiku
 *  reads and writes as well. */
constexpr uint32_t kDuplicateNode = 2;
constexpr uint32_t kDuplicateFragment = 3;
constexpr uint32_t kNumDuplicateValues = 125;

inline uint32_t link_type(int64_t link) noexcept
{
    return static_cast<uint64_t>(link) >> 62;
}

inline bool link_is_duplicate(int64_t link) noexcept
{
    return (link_type(link) & (kDuplicateNode | kDuplicateFragment)) != 0;
}

/** The node offset a link names (the low ten bits are a fragment index, which
 *  Aegir does not use). */
inline uint64_t link_offset(int64_t link) noexcept
{
    return static_cast<uint64_t>(link) & 0x3ffffffffffffc00ULL;
}

inline int64_t make_link(uint32_t type, uint64_t offset) noexcept
{
    return static_cast<int64_t>((static_cast<uint64_t>(type) << 62) |
                                (offset & 0x3ffffffffffffc00ULL));
}

/** A key in a tree is at most this many bytes; an attribute name too. */
constexpr uint32_t kMaxName = 255;
constexpr uint32_t kMaxKeyLength = 256;

/** The end of a run list, and the end of a tree's links. */
constexpr int64_t kNullLink = -1;
constexpr int64_t kFreeLink = -2;

/** The root directory's entry in a tree, and the file-name small data's
 *  fixed name: the one-byte 0x13. */
constexpr uint8_t kFileNameName = 0x13;
constexpr uint32_t kFileNameType = 0x43535452; /* 'CSTR' */

/** One block run, eight bytes packed. */
struct Run {
    uint32_t allocation_group;
    uint16_t start;
    uint16_t length;
};

inline uint16_t le16(uint8_t const *at) noexcept
{
    return static_cast<uint16_t>(at[0]) | (static_cast<uint16_t>(at[1]) << 8);
}

inline uint32_t le32(uint8_t const *at) noexcept
{
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
           (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
}

inline uint64_t le64(uint8_t const *at) noexcept
{
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(at[i]) << (i * 8);
    }
    return value;
}

inline int64_t le64_signed(uint8_t const *at) noexcept
{
    return static_cast<int64_t>(le64(at));
}

inline Run le_run(uint8_t const *at) noexcept
{
    Run run;
    run.allocation_group = le32(at);
    run.start = le16(at + 4);
    run.length = le16(at + 6);
    return run;
}

inline void put_le16(uint8_t *at, uint16_t value) noexcept
{
    at[0] = static_cast<uint8_t>(value & 0xff);
    at[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

inline void put_le32(uint8_t *at, uint32_t value) noexcept
{
    for (uint32_t i = 0; i < 4; ++i) {
        at[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
    }
}

inline void put_le64(uint8_t *at, uint64_t value) noexcept
{
    for (uint32_t i = 0; i < 8; ++i) {
        at[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
    }
}

inline void put_run(uint8_t *at, Run const &run) noexcept
{
    put_le32(at, run.allocation_group);
    put_le16(at + 4, run.start);
    put_le16(at + 6, run.length);
}

inline Run run_make(uint32_t allocation_group, uint16_t start,
                    uint16_t length) noexcept
{
    Run run;
    run.allocation_group = allocation_group;
    run.start = start;
    run.length = length;
    return run;
}

/** True when two runs sit one after the other on the same allocation group,
 *  which is what lets an append merge them into one. */
inline bool runs_contiguous(Run const &a, Run const &b) noexcept
{
    return a.allocation_group == b.allocation_group &&
           static_cast<uint32_t>(a.start) + a.length == b.start;
}

inline bool run_is_zero(Run const &run) noexcept
{
    return run.allocation_group == 0 && run.start == 0 && run.length == 0;
}

/** The superblock's field offsets (relative to the struct, which is at
 *  kSuperblockOffset of the volume). */
namespace superblock {
constexpr uint32_t kName = 0;           /* 32 bytes */
constexpr uint32_t kMagic1 = 32;
constexpr uint32_t kByteOrder = 36;
constexpr uint32_t kBlockSize = 40;
constexpr uint32_t kBlockShift = 44;
constexpr uint32_t kNumBlocks = 48;
constexpr uint32_t kUsedBlocks = 56;
constexpr uint32_t kInodeSize = 64;
constexpr uint32_t kMagic2 = 68;
constexpr uint32_t kBlocksPerAg = 72;
constexpr uint32_t kAgShift = 76;
constexpr uint32_t kNumAgs = 80;
constexpr uint32_t kFlags = 84;
constexpr uint32_t kLog = 88;        /* a block_run */
constexpr uint32_t kLogStart = 96;
constexpr uint32_t kLogEnd = 104;
constexpr uint32_t kMagic3 = 112;
constexpr uint32_t kRootDir = 116;   /* a block_run */
constexpr uint32_t kIndices = 124;   /* a block_run */
constexpr uint32_t kAegirFlags = 132; /* _reserved[0]: this spec's gate */
}  // namespace superblock

/** The inode's field offsets, relative to the inode block. */
namespace inode {
constexpr uint32_t kMagic1 = 0;
constexpr uint32_t kInodeNum = 4;      /* a block_run */
constexpr uint32_t kUid = 12;
constexpr uint32_t kGid = 16;
constexpr uint32_t kMode = 20;
constexpr uint32_t kFlags = 24;
constexpr uint32_t kCreateTime = 28;
constexpr uint32_t kLastModified = 36;
constexpr uint32_t kParent = 44;       /* a block_run */
constexpr uint32_t kAttributes = 52;   /* a block_run */
constexpr uint32_t kType = 60;
constexpr uint32_t kInodeSize = 64;
constexpr uint32_t kEtc = 68;
constexpr uint32_t kData = 72;         /* the data_stream */
constexpr uint32_t kSmallData = 232;
}  // namespace inode

/** The data stream's field offsets, relative to inode::kData. */
namespace data {
constexpr uint32_t kDirect = 0;                  /* 12 block_runs */
constexpr uint32_t kDirectCount = 12;
constexpr uint32_t kMaxDirectRange = 96;
constexpr uint32_t kIndirect = 104;              /* a block_run */
constexpr uint32_t kMaxIndirectRange = 112;
constexpr uint32_t kDoubleIndirect = 120;        /* a block_run */
constexpr uint32_t kMaxDoubleIndirectRange = 128;
constexpr uint32_t kSize = 136;
constexpr uint32_t kBytes = 144;
}  // namespace data

/** The B+tree header's field offsets, relative to the tree stream's start. */
namespace tree_header {
constexpr uint32_t kMagic = 0;
constexpr uint32_t kNodeSize = 4;
constexpr uint32_t kMaxLevels = 8;
constexpr uint32_t kDataType = 12;
constexpr uint32_t kRootNode = 16;
constexpr uint32_t kFreeNode = 24;
constexpr uint32_t kMaximumSize = 32;
constexpr uint32_t kBytes = 40;
}  // namespace tree_header

/** The B+tree node's field offsets, relative to the node's start. */
namespace node {
constexpr uint32_t kLeftLink = 0;
constexpr uint32_t kRightLink = 8;
constexpr uint32_t kOverflowLink = 16;
constexpr uint32_t kKeyCount = 24;
constexpr uint32_t kAllKeyLength = 26;
constexpr uint32_t kFixed = 28;
}  // namespace node

/** The journal's run array: a block-sized header listing, in ascending block
 *  order, the target blocks a transaction changed. Its size equals the block
 *  size, so a freshly allocated one is ready to use. Be's replay can only
 *  handle runs of length one, so every run here is one block. */
namespace run_array {
constexpr uint32_t kCount = 0;
constexpr uint32_t kMaxRuns = 4;
constexpr uint32_t kRuns = 8;
constexpr uint32_t kFixed = 8; /* sizeof(run_array) */

/** The `max_runs` field: Haiku caps it at 127 no matter the block size
 *  (run_array::MaxRuns), and its usable count is one less, for an off-by-one
 *  in Be's own implementation. */
inline uint32_t max_runs(uint32_t block_size) noexcept
{
    uint32_t const fits = (block_size - kFixed) / 8; /* sizeof(block_run) */
    return fits < 128 ? fits : 127;
}
}  // namespace run_array

/** Round up to the next off_t boundary, as key_align does. */
inline uint32_t key_align(uint32_t value) noexcept
{
    return (value + 7) & ~7u;
}

}  // namespace aegir::bfs

#endif  // AEGIR_BFS_LAYOUT_H
