/*
 * The GUID partition table's on-disk words, little-endian throughout.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "gpt.h"

namespace aegir::gpt {

namespace {

constexpr uint32_t kSectorBytes = 512;
constexpr uint32_t kMbrSignature = 510;   /* 0x55, 0xAA */
constexpr uint32_t kMbrFirstEntry = 446;  /* the first of four 16-byte entries */
constexpr uint8_t kMbrGptType = 0xee;

uint64_t word64(uint8_t const *at) noexcept
{
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(at[i]) << (i * 8);
    }
    return value;
}

uint32_t word32(uint8_t const *at) noexcept
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(at[i]) << (i * 8);
    }
    return value;
}

}  // namespace

bool protective_mbr(uint8_t const *sector) noexcept
{
    if (sector[kMbrSignature] != 0x55 || sector[kMbrSignature + 1] != 0xaa) {
        return false;
    }
    /* The protective entry is the first one by convention; any of the four
     * saying 0xEE is what the specification actually asks for. */
    for (uint32_t e = 0; e < 4; ++e) {
        if (sector[kMbrFirstEntry + e * 16 + 4] == kMbrGptType) {
            return true;
        }
    }
    return false;
}

bool header(uint8_t const *sector, uint64_t *entries_lba, uint32_t *entry_count,
            uint32_t *entry_bytes) noexcept
{
    static char const kSignature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    for (uint32_t i = 0; i < 8; ++i) {
        if (sector[i] != static_cast<uint8_t>(kSignature[i])) {
            return false;
        }
    }
    if (entries_lba != nullptr) {
        *entries_lba = word64(sector + 72);
    }
    if (entry_count != nullptr) {
        *entry_count = word32(sector + 80);
    }
    if (entry_bytes != nullptr) {
        *entry_bytes = word32(sector + 84);
    }
    return true;
}

bool entry(uint8_t const *raw, uint32_t bytes, Partition *partition) noexcept
{
    if (bytes < 128) {
        return false;
    }
    bool used = false;
    for (uint32_t i = 0; i < 16; ++i) {
        if (raw[i] != 0) {
            used = true;
            break;
        }
    }
    if (!used) {
        return false;
    }
    for (uint32_t i = 0; i < 16; ++i) {
        partition->type[i] = raw[i];
    }
    partition->first_lba = word64(raw + 32);
    partition->last_lba = word64(raw + 40);
    /* The name is 36 UTF-16LE code units. Narrow it: ASCII arrives as itself,
     * and the first unit outside it ends the name rather than being
     * transliterated into a lie. */
    partition->name_length = 0;
    for (uint32_t i = 0; i < 36; ++i) {
        uint8_t const low = raw[56 + i * 2];
        uint8_t const high = raw[56 + i * 2 + 1];
        if (high != 0 || low == 0) {
            break;
        }
        partition->name[partition->name_length++] = static_cast<char>(low);
    }
    return true;
}

}  // namespace aegir::gpt
