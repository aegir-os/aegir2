/*
 * The FAT formats' on-disk words, little-endian throughout.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "fat.h"

namespace aegir::fat {

namespace {

uint16_t word16(uint8_t const *at) noexcept
{
    return static_cast<uint16_t>(at[0] | (static_cast<uint16_t>(at[1]) << 8));
}

uint32_t word32(uint8_t const *at) noexcept
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(at[i]) << (i * 8);
    }
    return value;
}

void put16(uint8_t *at, uint16_t value) noexcept
{
    at[0] = static_cast<uint8_t>(value);
    at[1] = static_cast<uint8_t>(value >> 8);
}

void put32(uint8_t *at, uint32_t value) noexcept
{
    for (uint32_t i = 0; i < 4; ++i) {
        at[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

}  // namespace

bool bpb(uint8_t const *sector, Volume *volume) noexcept
{
    if (sector[510] != 0x55 || sector[511] != 0xaa) {
        return false;
    }
    uint16_t const bytes_per_sector = word16(sector + 11);
    uint8_t const sectors_per_cluster = sector[13];
    uint16_t const reserved = word16(sector + 14);
    uint8_t const fats = sector[16];
    uint16_t const root_entries = word16(sector + 17);
    if (bytes_per_sector != 512 || sectors_per_cluster == 0 || reserved == 0 ||
        fats == 0) {
        return false;
    }
    /* Root entries is the field that tells the two layouts apart: FAT32 moved
     * the root into a cluster chain and zeroed it. */
    volume->fat32 = root_entries == 0;
    uint32_t const fat_sectors =
        volume->fat32 ? word32(sector + 36) : word16(sector + 22);
    if (fat_sectors == 0) {
        return false;
    }
    volume->sectors_per_cluster = sectors_per_cluster;
    volume->fat_start = reserved;
    volume->fat_sectors = fat_sectors;
    volume->fats = fats;
    volume->root_sectors =
        volume->fat32 ? 0u : (static_cast<uint32_t>(root_entries) * 32 + 511) / 512;
    volume->root_start = static_cast<uint64_t>(reserved) +
                         static_cast<uint64_t>(fats) * fat_sectors;
    volume->data_start = volume->root_start + volume->root_sectors;
    volume->root_cluster = volume->fat32 ? word32(sector + 44) : 0;
    return true;
}

uint64_t cluster_sector(Volume const &volume, uint32_t cluster) noexcept
{
    /* Clusters count from 2: the first data sector is cluster 2's. */
    return volume.data_start +
           static_cast<uint64_t>(cluster - 2) * volume.sectors_per_cluster;
}

Entry dirent(uint8_t const *raw, Dirent *out) noexcept
{
    if (raw[0] == 0x00) {
        return Entry::End;
    }
    if (raw[0] == 0xe5 || raw[11] == 0x0f || (raw[11] & 0x08) != 0) {
        /* Deleted, a long-name fragment, or the volume label. */
        return Entry::Skip;
    }
    /* 8.3, padded with spaces: trim them, and join with a dot only when there
     * is an extension. */
    uint32_t base = 8;
    while (base > 0 && raw[base - 1] == ' ') {
        --base;
    }
    uint32_t ext = 11;
    while (ext > 8 && raw[ext - 1] == ' ') {
        --ext;
    }
    out->name_length = 0;
    for (uint32_t i = 0; i < base; ++i) {
        out->name[out->name_length++] = static_cast<char>(raw[i]);
    }
    if (ext > 8) {
        out->name[out->name_length++] = '.';
        for (uint32_t i = 8; i < ext; ++i) {
            out->name[out->name_length++] = static_cast<char>(raw[i]);
        }
    }
    out->first_cluster =
        (static_cast<uint32_t>(word16(raw + 20)) << 16) | word16(raw + 26);
    out->bytes = word32(raw + 28);
    out->directory = (raw[11] & 0x10) != 0;
    return Entry::Used;
}

uint32_t next32(uint8_t const *fat_sector, uint32_t cluster_mod_128) noexcept
{
    /* The high four bits are reserved and must be masked off (Microsoft's FAT
     * specification, "FAT32 Directory Entry" -- the entry is 28 bits). */
    return word32(fat_sector + cluster_mod_128 * 4) & 0x0fffffffu;
}

uint32_t next16(uint8_t const *fat_sector, uint32_t cluster_mod_256) noexcept
{
    return word16(fat_sector + cluster_mod_256 * 2);
}

bool name_83(char const *name, uint32_t length, uint8_t out[11]) noexcept
{
    uint32_t dot = length;
    for (uint32_t i = 0; i < length; ++i) {
        if (name[i] == '.') {
            if (dot != length) {
                return false; /* one dot is the whole form */
            }
            dot = i;
        }
    }
    uint32_t const base = dot;
    uint32_t const ext = dot == length ? 0 : length - dot - 1;
    if (base == 0 || base > 8 || ext > 3 || (dot != length && ext == 0)) {
        return false;
    }
    for (uint32_t i = 0; i < 11; ++i) {
        out[i] = ' ';
    }
    for (uint32_t i = 0; i < base; ++i) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - ('a' - 'A'));
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
        out[i] = static_cast<uint8_t>(c);
    }
    for (uint32_t i = 0; i < ext; ++i) {
        char c = name[dot + 1 + i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - ('a' - 'A'));
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
        out[8 + i] = static_cast<uint8_t>(c);
    }
    return true;
}

void dirent_make(uint8_t slot[32], uint8_t const name83[11]) noexcept
{
    for (uint32_t i = 0; i < 32; ++i) {
        slot[i] = 0;
    }
    for (uint32_t i = 0; i < 11; ++i) {
        slot[i] = name83[i];
    }
    slot[kDirentAttr] = kAttrArchive;
}

void dirent_make_dir(uint8_t slot[32], uint8_t const name83[11],
                     uint32_t cluster) noexcept
{
    dirent_make(slot, name83);
    slot[kDirentAttr] = kAttrDirectory;
    put16(slot + kDirentClusterHigh, static_cast<uint16_t>(cluster >> 16));
    put16(slot + kDirentClusterLow, static_cast<uint16_t>(cluster));
}

void dirent_update(uint8_t slot[32], uint32_t first_cluster, uint32_t bytes) noexcept
{
    put16(slot + kDirentClusterHigh, static_cast<uint16_t>(first_cluster >> 16));
    put16(slot + kDirentClusterLow, static_cast<uint16_t>(first_cluster));
    put32(slot + kDirentSize, bytes);
}

void set_next32(uint8_t *fat_sector, uint32_t cluster_mod_128, uint32_t value) noexcept
{
    /* The high four bits are reserved and kept (next32 masks them off when
     * reading), so the write is a read-modify-write of the whole entry. */
    uint8_t *entry = fat_sector + cluster_mod_128 * 4;
    put32(entry, (word32(entry) & 0xf0000000u) | (value & 0x0fffffffu));
}

void set_next16(uint8_t *fat_sector, uint32_t cluster_mod_256, uint32_t value) noexcept
{
    put16(fat_sector + cluster_mod_256 * 2, static_cast<uint16_t>(value));
}

}  // namespace aegir::fat
