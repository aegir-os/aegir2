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

/** Which layout a boot sector describes. The walk speaks FAT16 and FAT32;
 *  FAT12 and ExFAT are recognized so the refusal can name them rather than
 *  calling a real format "not a BPB". */
enum class Flavor : uint32_t {
    NotFat, /* no FAT boot sector here at all */
    Fat12,  /* a FAT12 volume: recognized, not spoken */
    Fat16,
    Fat32,
    Exfat, /* an ExFAT boot sector: recognized, not spoken */
};

/** What the BPB says, boiled down to what a reader needs. Sector numbers are
 *  relative to the volume. */
struct Volume {
    uint32_t sectors_per_cluster;
    uint64_t fat_start;   /* the first FAT's first sector */
    uint32_t fat_sectors; /* per FAT */
    uint32_t fats;        /* how many copies a change must write */
    uint64_t data_start;  /* the first data sector: cluster 2 lives here */
    uint32_t root_cluster; /* FAT32: where the root directory's chain starts */
    uint64_t root_start;   /* FAT16: the root region's first sector */
    uint32_t root_sectors; /* FAT16: how long the root region is */
    bool fat32;
};

/** Parse the BPB at the volume's sector 0. `NotFat` when it is not one; the
 *  other flavors fill `volume` (ExFAT alone leaves it unset, its fields
 *  living elsewhere) so a caller can report which format it refused. */
Flavor bpb(uint8_t const *sector, Volume *volume) noexcept;

/** A cluster's first sector, volume-relative. */
uint64_t cluster_sector(Volume const &volume, uint32_t cluster) noexcept;

/** The longest a long name may be, in UTF-16 code units, and the UTF-8 buffer
 *  that holds its widest encoding: three bytes per unit for the BMP, which is
 *  the most a code unit can expand to (a surrogate pair spends two units on
 *  four bytes, so it is smaller). */
constexpr uint32_t kLongNameUnits = 255;
constexpr uint32_t kLongNameBytes = 3 * kLongNameUnits;

/** One directory entry, narrowed to what a listing shows: the name as it is
 *  meant to be read -- the long form where one exists, else the 8.3 form --
 *  the first cluster, and the size. */
struct Dirent {
    char name[kLongNameBytes];
    uint32_t name_length;
    uint32_t first_cluster;
    uint32_t bytes;
    bool directory;
};

/** What one 32-byte slot is, before a long-name run is considered. */
enum class SlotKind : uint32_t {
    End,     /* a zero first byte: nothing past here is used */
    Deleted, /* 0xe5 in the first byte: the slot is free */
    Lfn,     /* a long-name fragment (attribute 0x0f) */
    Label,   /* the volume label (attribute 0x08) */
    Short,   /* a file or directory entry */
};

/** Classify one slot by its leading byte and attribute. */
SlotKind slot_kind(uint8_t const *raw) noexcept;

/** Read a short slot's fields. The name is its 8.3 form; the scanner
 *  substitutes a long name when a valid run precedes it (Lfn, below). */
void short_dirent(uint8_t const *raw, Dirent *out) noexcept;

/** A long-name run under assembly. Its slots precede the 8.3 slot they belong
 *  to, stored tail first: reading forward, the slot with the highest sequence
 *  number (and `0x40` in its first byte) comes first, sequence one last. Feed
 *  each fragment as it is read, then ask whether the run belongs to the 8.3
 *  name that follows. */
struct Lfn {
    uint16_t units[kLongNameUnits];
    uint32_t count;    /* how many units the run's slots claim */
    uint32_t expected; /* the sequence number the next slot must carry; 0 done */
    uint8_t checksum;  /* the 8.3 name's checksum, from every slot of the run */
    bool active;
};

/** Start an empty run. */
void lfn_reset(Lfn *run) noexcept;

/** Feed one fragment (a slot `slot_kind` calls `Lfn`). A fragment with no head
 *  or out of order discards the run rather than adopting the wrong name. */
void lfn_feed(Lfn *run, uint8_t const *slot) noexcept;

/** True when the run is complete and is this 8.3 name's: the checksums agree. */
bool lfn_matches(Lfn const &run, uint8_t const *short_name) noexcept;

/** Decode a complete run to UTF-8, stopping at the name's terminator. The
 *  answer is the byte count; the buffer must hold `kLongNameBytes`. */
uint32_t lfn_decode(Lfn const &run, char *out) noexcept;

/** End of a cluster chain is a range of marks, not one sentinel. */
constexpr uint32_t kEoc32 = 0x0ffffff8;
constexpr uint32_t kEoc16 = 0xfff8;

/** The next cluster in a chain, from the FAT sector the caller fetched:
 *  4-byte entries on FAT32 (128 to a sector), 2-byte on FAT16 (256). */
uint32_t next32(uint8_t const *fat_sector, uint32_t cluster_mod_128) noexcept;
uint32_t next16(uint8_t const *fat_sector, uint32_t cluster_mod_256) noexcept;

/* The write side. A free cluster's FAT entry is zero; the value written at
 * a chain's end is the EOC mark (any value at or past the floor reads as
 * end -- this is the one the tools write). */
constexpr uint32_t kFreeCluster = 0;
constexpr uint32_t kEocMark32 = 0x0fffffff;
constexpr uint32_t kEocMark16 = 0xffff;

/** A 32-byte directory slot's fields, as offsets. */
constexpr uint32_t kDirentAttr = 11;
constexpr uint32_t kDirentClusterHigh = 20;
constexpr uint32_t kDirentClusterLow = 26;
constexpr uint32_t kDirentSize = 28;
constexpr uint8_t kAttrArchive = 0x20;
constexpr uint8_t kAttrDirectory = 0x10;

/** Validate a client's name and build its 8.3 form: uppercase, space-padded,
 *  one dot, letters and digits and '-' and '_' (the conservative end of the
 *  format's charset -- a name past it is refused, not mangled). */
bool name_83(char const *name, uint32_t length, uint8_t out[11]) noexcept;

/** Fill a 32-byte slot: a plain new file of this name. */
void dirent_make(uint8_t slot[32], uint8_t const name83[11]) noexcept;

/** Fill a 32-byte slot: a new directory of this name, on this cluster. */
void dirent_make_dir(uint8_t slot[32], uint8_t const name83[11],
                     uint32_t cluster) noexcept;

/** Patch an existing slot after a write: the first cluster and the size. */
void dirent_update(uint8_t slot[32], uint32_t first_cluster, uint32_t bytes) noexcept;

/** Set one entry in a FAT sector the caller holds (the mirror of next*). */
void set_next32(uint8_t *fat_sector, uint32_t cluster_mod_128, uint32_t value) noexcept;
void set_next16(uint8_t *fat_sector, uint32_t cluster_mod_256, uint32_t value) noexcept;

}  // namespace aegir::fat
