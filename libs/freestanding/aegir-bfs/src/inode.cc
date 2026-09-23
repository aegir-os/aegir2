/*
 * Building and patching a Be File System inode block.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/bfs/inode.h>

namespace aegir::bfs {

namespace {

void zero(uint8_t *block, uint32_t block_size) noexcept
{
    for (uint32_t i = 0; i < block_size; ++i) {
        block[i] = 0;
    }
}

void write_small_name(uint8_t *block, uint32_t block_size, char const *name,
                      uint32_t name_length) noexcept
{
    uint32_t const at = inode::kSmallData;
    /* A small_data entry is 12 + name_size + data_size bytes: the eight-byte
     * header, the name, three pad bytes, the data, and a terminator. Data()
     * is Name() + NameSize() + 3, exactly as Haiku's small_data::Data()
     * computes it, so a Haiku reader finds the file name. */
    if (name_length == 0 || name_length > kMaxName ||
        at + 13 + name_length > block_size) {
        return;
    }
    uint8_t *entry = block + at;
    put_le32(entry, kFileNameType);
    put_le16(entry + 4, 1); /* FILE_NAME_NAME_LENGTH */
    put_le16(entry + 6, static_cast<uint16_t>(name_length));
    entry[8] = kFileNameName;
    entry[9] = 0;
    entry[10] = 0;
    entry[11] = 0;
    for (uint32_t i = 0; i < name_length; ++i) {
        entry[12 + i] = static_cast<uint8_t>(name[i]);
    }
    entry[12 + name_length] = 0;
}

}  // namespace

void inode_build(uint8_t *block, uint32_t block_size, Run const &run,
                 Run const &parent, uint32_t mode, int64_t time, char const *name,
                 uint32_t name_length) noexcept
{
    zero(block, block_size);
    put_le32(block + inode::kMagic1, kInodeMagic1);
    put_run(block + inode::kInodeNum, run);
    put_le32(block + inode::kUid, 0);
    put_le32(block + inode::kGid, 0);
    put_le32(block + inode::kMode, mode);
    put_le32(block + inode::kFlags, kInodeInUse);
    put_le64(block + inode::kCreateTime, static_cast<uint64_t>(time));
    put_le64(block + inode::kLastModified, static_cast<uint64_t>(time));
    put_run(block + inode::kParent, parent);
    put_run(block + inode::kAttributes, Run{0, 0, 0});
    put_le32(block + inode::kType, 0);
    put_le32(block + inode::kInodeSize, block_size);
    put_le32(block + inode::kEtc, 0);
    /* the data stream stays zero, and nothing is allocated yet */
    put_le64(block + inode::kData + data::kSize, 0);
    put_le64(block + inode::kData + data::kBytes, static_cast<uint64_t>(time));
    write_small_name(block, block_size, name, name_length);
}

void inode_get_stream(uint8_t const *block, uint8_t *stream) noexcept
{
    for (uint32_t i = 0; i < data::kBytes; ++i) {
        stream[i] = block[inode::kData + i];
    }
}

void inode_set_stream(uint8_t *block, uint8_t const *stream, int64_t size,
                      int64_t mtime) noexcept
{
    for (uint32_t i = 0; i < data::kBytes; ++i) {
        block[inode::kData + i] = stream[i];
    }
    put_le64(block + inode::kData + data::kSize, static_cast<uint64_t>(size));
    put_le64(block + inode::kLastModified, static_cast<uint64_t>(mtime));
    put_le64(block + inode::kData + data::kBytes, static_cast<uint64_t>(mtime));
}

void inode_mark_deleted(uint8_t *block) noexcept
{
    put_le32(block + inode::kMagic1, 0);
    put_le32(block + inode::kFlags, kInodeDeleted);
}

}  // namespace aegir::bfs