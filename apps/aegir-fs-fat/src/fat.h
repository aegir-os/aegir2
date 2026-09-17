/*
 * Reading a FAT volume.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The format only: what the BPB says, where it puts the FATs and the data,
 * and what one directory entry holds. FAT16 and FAT32 are the same walk with
 * different numbers -- the root is a fixed region on one and a cluster chain
 * like any other on the second. Whoever reads sectors hands them in; nothing
 * here knows how a sector was fetched (specs/services.md).
 */

#pragma once

#include <stdint.h>

namespace aegir::fat {

/** What the BPB says, boiled down to what a reader needs. Sector numbers are
 *  relative to the volume. */
struct Volume {
    uint32_t sectors_per_cluster;
    uint64_t fat_start;   /* the first FAT's first sector */
    uint32_t fat_sectors; /* per FAT */
    uint64_t data_start;  /* the first data sector: cluster 2 lives here */
    uint32_t root_cluster; /* FAT32: where the root directory's chain starts */
    uint64_t root_start;   /* FAT16: the root region's first sector */
    uint32_t root_sectors; /* FAT16: how long the root region is */
    bool fat32;
};

/** Parse the BPB at the volume's sector 0. False when it is not one. */
bool bpb(uint8_t const *sector, Volume *volume) noexcept;

/** A cluster's first sector, volume-relative. */
uint64_t cluster_sector(Volume const &volume, uint32_t cluster) noexcept;

/** One directory entry, narrowed to what a listing shows: the 8.3 name as
 *  "NAME.EXT", the first cluster, and the size. */
struct Dirent {
    char name[13];
    uint32_t name_length;
    uint32_t first_cluster;
    uint32_t bytes;
    bool directory;
};

/** What one 32-byte slot turned out to be. */
enum class Entry : uint32_t {
    End,  /* a zero first byte: the directory is over */
    Skip, /* deleted, a long-name fragment, or the volume label */
    Used,
};

/** Read one 32-byte slot. */
Entry dirent(uint8_t const *raw, Dirent *out) noexcept;

/** End of a cluster chain is a range of marks, not one sentinel. */
constexpr uint32_t kEoc32 = 0x0ffffff8;

/** The next cluster in a chain, from the FAT sector the caller fetched:
 *  `cluster_mod_128` is the cluster's index within it (4-byte entries, 128 to
 *  a sector). */
uint32_t next32(uint8_t const *fat_sector, uint32_t cluster_mod_128) noexcept;

}  // namespace aegir::fat
