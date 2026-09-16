/*
 * Reading a GUID partition table.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The format only: what a protective MBR looks like, what the GPT header at
 * LBA 1 says, and what one partition entry holds. Whoever reads sectors hands
 * them in; nothing here knows how a sector was fetched, because the partition
 * manager's job is the walk, not the transport (specs/services.md).
 */

#pragma once

#include <stdint.h>

namespace aegir::gpt {

/** One partition, read out of an entry. */
struct Partition {
    uint8_t type[16];  /* the type GUID, as the entry holds it */
    uint64_t first_lba;
    uint64_t last_lba;
    char name[36]; /* the entry's UTF-16 name, narrowed to ASCII */
    uint32_t name_length;
};

/** True when sector 0 is a protective MBR: the boot signature at its end, and
 *  a 0xEE entry covering the disk -- the sign a GPT follows at LBA 1. */
bool protective_mbr(uint8_t const *sector) noexcept;

/** The header at LBA 1: false when the signature is not "EFI PART". On
 *  success, where the entries start and how they are sized. */
bool header(uint8_t const *sector, uint64_t *entries_lba, uint32_t *entry_count,
            uint32_t *entry_bytes) noexcept;

/** One entry: false when it is unused, which is a zero type GUID. */
bool entry(uint8_t const *raw, uint32_t bytes, Partition *partition) noexcept;

}  // namespace aegir::gpt
