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

/* One byte of a generated 8.3 alias: a letter kept and uppercased, a digit,
 * or '-' and '_' kept, and anything else -- a space, punctuation, a UTF-8
 * continuation byte -- replaced by '_' (the conservative end of the format's
 * charset, the way Windows' own generator mangles rather than refuses). */
char alias_char(uint8_t c) noexcept
{
    if (c >= 'a' && c <= 'z') {
        return static_cast<char>(c - ('a' - 'A'));
    }
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
        return static_cast<char>(c);
    }
    return '_';
}

}  // namespace

/* The VFAT checksum of an 8.3 name, the one every slot of a long-name run
 * carries so a stale run cannot be adopted by a neighbour. */
uint8_t name_checksum(uint8_t const short_name[11]) noexcept
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 11; ++i) {
        sum = static_cast<uint8_t>(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    }
    return sum;
}

Flavor bpb(uint8_t const *sector, Volume *volume) noexcept
{
    /* ExFAT is a different format wearing a FAT name: its boot sector says so
     * at offset 3, and its fields live somewhere else entirely. Name it before
     * the FAT layout is read, so the refusal can say which format it was. */
    if (sector[3] == 'E' && sector[4] == 'X' && sector[5] == 'F' && sector[6] == 'A' &&
        sector[7] == 'T' && sector[8] == ' ') {
        return Flavor::Exfat;
    }
    if (sector[510] != 0x55 || sector[511] != 0xaa) {
        return Flavor::NotFat;
    }
    uint16_t const bytes_per_sector = word16(sector + 11);
    uint8_t const sectors_per_cluster = sector[13];
    uint16_t const reserved = word16(sector + 14);
    uint8_t const fats = sector[16];
    uint16_t const root_entries = word16(sector + 17);
    if (bytes_per_sector != 512 || sectors_per_cluster == 0 || reserved == 0 ||
        fats == 0) {
        return Flavor::NotFat;
    }
    /* Root entries is the field that tells the two layouts apart: FAT32 moved
     * the root into a cluster chain and zeroed it. */
    volume->fat32 = root_entries == 0;
    uint32_t const fat_sectors =
        volume->fat32 ? word32(sector + 36) : word16(sector + 22);
    if (fat_sectors == 0) {
        return Flavor::NotFat;
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

    /* Which FAT is here, by the format's own rule (Microsoft's FAT
     * specification, "Determination of FAT Type"): under 4085 clusters is
     * FAT12, under 65525 is FAT16, and the rest is FAT32. The root's shape
     * wins when the two would disagree, because that is what the walk
     * actually follows -- a small volume whose root is a chain is still
     * FAT32's layout. */
    if (volume->fat32) {
        return Flavor::Fat32;
    }
    uint32_t const total_sectors =
        word16(sector + 19) != 0 ? word16(sector + 19) : word32(sector + 32);
    uint32_t const data_sectors =
        total_sectors > volume->data_start ? total_sectors - volume->data_start : 0;
    uint32_t const clusters = data_sectors / volume->sectors_per_cluster;
    return clusters < 4085 ? Flavor::Fat12 : Flavor::Fat16;
}

uint64_t cluster_sector(Volume const &volume, uint32_t cluster) noexcept
{
    /* Clusters count from 2: the first data sector is cluster 2's. */
    return volume.data_start +
           static_cast<uint64_t>(cluster - 2) * volume.sectors_per_cluster;
}

SlotKind slot_kind(uint8_t const *raw) noexcept
{
    if (raw[0] == 0x00) {
        return SlotKind::End;
    }
    if (raw[0] == 0xe5) {
        return SlotKind::Deleted;
    }
    if (raw[11] == 0x0f) {
        return SlotKind::Lfn;
    }
    if ((raw[11] & 0x08) != 0) {
        return SlotKind::Label;
    }
    return SlotKind::Short;
}

void short_dirent(uint8_t const *raw, Dirent *out) noexcept
{
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
    out->date = word16(raw + kDirentWriteDate);
    out->time = word16(raw + kDirentWriteTime);
    out->directory = (raw[11] & 0x10) != 0;
}

void lfn_reset(Lfn *run) noexcept
{
    run->count = 0;
    run->expected = 0;
    run->checksum = 0;
    run->active = false;
}

void lfn_feed(Lfn *run, uint64_t sector, uint32_t index, uint8_t const *slot) noexcept
{
    /* The low six bits are the sequence number; 0x40 marks the run's first
     * physical slot (which holds the name's tail), 0x80 is never set on a
     * live slot. Twenty slots is the format's ceiling: 20*13 > 255. */
    uint8_t const field = slot[0];
    uint32_t const sequence = field & 0x3f;
    if (sequence == 0 || sequence > kLfnSlotsMax) {
        lfn_reset(run);
        return;
    }
    if ((field & 0x40) != 0) {
        /* The head slot names the run's length. */
        run->count = sequence * 13;
        run->expected = sequence - 1;
        run->checksum = slot[13];
        run->active = true;
    } else if (!run->active || sequence != run->expected) {
        /* A fragment with no head, or out of order: not this entry's run. */
        lfn_reset(run);
        return;
    } else {
        run->expected = sequence - 1;
    }
    /* Keep the fragment's place so a removal can delete the whole run. */
    run->fragments[sequence - 1] = sector;
    run->fragment_index[sequence - 1] = index;
    /* Place the slot's thirteen units by sequence number: the first physical
     * slot is the name's last chunk, so it lands last. */
    static uint32_t const offsets[3] = {1, 14, 28};
    static uint32_t const counts[3] = {5, 6, 2};
    uint32_t unit = (sequence - 1) * 13;
    for (uint32_t g = 0; g < 3; ++g) {
        for (uint32_t j = 0; j < counts[g]; ++j) {
            uint16_t const lo = slot[offsets[g] + j * 2];
            uint16_t const hi = slot[offsets[g] + j * 2 + 1];
            if (unit < kLongNameUnits) {
                run->units[unit] = static_cast<uint16_t>(lo | (hi << 8));
            }
            ++unit;
        }
    }
}

bool lfn_matches(Lfn const &run, uint8_t const *short_name) noexcept
{
    if (!run.active || run.expected != 0 || run.count == 0) {
        return false;
    }
    return name_checksum(short_name) == run.checksum;
}

uint32_t lfn_decode(Lfn const &run, char *out) noexcept
{
    uint32_t bytes = 0;
    uint32_t const units = run.count < kLongNameUnits ? run.count : kLongNameUnits;
    for (uint32_t i = 0; i < units; ++i) {
        uint32_t const unit = run.units[i];
        if (unit == 0x0000) {
            break; /* the name's terminator */
        }
        if (unit == 0xffff) {
            continue; /* padding */
        }
        if (unit < 0x80) {
            out[bytes++] = static_cast<char>(unit);
        } else if (unit < 0x800) {
            out[bytes++] = static_cast<char>(0xc0 | (unit >> 6));
            out[bytes++] = static_cast<char>(0x80 | (unit & 0x3f));
        } else if (unit >= 0xd800 && unit < 0xdc00) {
            /* A high surrogate pairs with the next unit into a code point. */
            if (i + 1 < units && run.units[i + 1] >= 0xdc00 && run.units[i + 1] < 0xe000) {
                uint32_t const point =
                    0x10000 + ((unit - 0xd800) << 10) + (run.units[i + 1] - 0xdc00);
                out[bytes++] = static_cast<char>(0xf0 | (point >> 18));
                out[bytes++] = static_cast<char>(0x80 | ((point >> 12) & 0x3f));
                out[bytes++] = static_cast<char>(0x80 | ((point >> 6) & 0x3f));
                out[bytes++] = static_cast<char>(0x80 | (point & 0x3f));
                ++i;
            } else {
                out[bytes++] = '?'; /* a lone surrogate is not a name */
            }
        } else if (unit >= 0xdc00 && unit < 0xe000) {
            out[bytes++] = '?';
        } else {
            out[bytes++] = static_cast<char>(0xe0 | (unit >> 12));
            out[bytes++] = static_cast<char>(0x80 | ((unit >> 6) & 0x3f));
            out[bytes++] = static_cast<char>(0x80 | (unit & 0x3f));
        }
    }
    return bytes;
}

bool name_units(char const *name, uint32_t length, uint16_t *units,
                uint32_t units_max, uint32_t *unit_count) noexcept
{
    if (length == 0 || name[0] == ' ' || name[length - 1] == ' ' ||
        name[length - 1] == '.') {
        return false; /* a name with no body, or a space or dot at its edge */
    }
    uint32_t at = 0;
    uint32_t count = 0;
    while (at < length) {
        uint8_t const lead = static_cast<uint8_t>(name[at]);
        uint32_t point = 0;
        uint32_t step = 1;
        if (lead < 0x80) {
            point = lead;
        } else if ((lead & 0xe0) == 0xc0) {
            point = lead & 0x1f;
            step = 2;
        } else if ((lead & 0xf0) == 0xe0) {
            point = lead & 0x0f;
            step = 3;
        } else if ((lead & 0xf8) == 0xf0) {
            point = lead & 0x07;
            step = 4;
        } else {
            return false; /* a stray continuation or an over-long lead byte */
        }
        if (at + step > length) {
            return false; /* truncated */
        }
        for (uint32_t i = 1; i < step; ++i) {
            uint8_t const cont = static_cast<uint8_t>(name[at + i]);
            if ((cont & 0xc0) != 0x80) {
                return false;
            }
            point = (point << 6) | (cont & 0x3f);
        }
        if (point < 0x20 || point == 0x7f) {
            return false; /* a control character */
        }
        /* The punctuation FAT reserves for its own syntax. */
        if (point == '"' || point == '*' || point == '/' || point == ':' ||
            point == '<' || point == '>' || point == '?' || point == '\\' ||
            point == '|') {
            return false;
        }
        at += step;
        if (point >= 0x10000) {
            if (count + 2 > units_max) {
                return false;
            }
            uint32_t const v = point - 0x10000;
            units[count++] = static_cast<uint16_t>(0xd800 + (v >> 10));
            units[count++] = static_cast<uint16_t>(0xdc00 + (v & 0x3ff));
        } else {
            if (count + 1 > units_max) {
                return false;
            }
            units[count++] = static_cast<uint16_t>(point);
        }
    }
    if (count == 0) {
        return false;
    }
    *unit_count = count;
    return true;
}

uint32_t lfn_build(uint16_t const *units, uint32_t unit_count, uint8_t checksum,
                   uint8_t *slots) noexcept
{
    /* Thirteen units to a slot; the physical order runs from the name's tail
     * (the head slot, marked 0x40) to its first units. */
    uint32_t const fragments = (unit_count + 12) / 13;
    static uint32_t const byte_offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (uint32_t p = 0; p < fragments; ++p) {
        uint32_t const sequence = fragments - p;
        uint8_t *slot = slots + p * 32;
        for (uint32_t i = 0; i < 32; ++i) {
            slot[i] = 0;
        }
        slot[0] = static_cast<uint8_t>(sequence | (p == 0 ? 0x40 : 0));
        slot[kDirentAttr] = 0x0f;
        slot[13] = checksum;
        uint32_t const base = (sequence - 1) * 13;
        for (uint32_t u = 0; u < 13; ++u) {
            uint32_t const index = base + u;
            uint16_t value = 0xffff; /* the pads between the name and the slot's end */
            if (index < unit_count) {
                value = units[index];
            } else if (index == unit_count) {
                value = 0x0000; /* the terminator, once */
            }
            slot[byte_offsets[u]] = static_cast<uint8_t>(value);
            slot[byte_offsets[u] + 1] = static_cast<uint8_t>(value >> 8);
        }
    }
    return fragments;
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

bool short_alias(char const *name, uint32_t length, uint32_t serial,
                 uint8_t out[11]) noexcept
{
    /* The serial's digits decide how much base fits: eight, less the '~',
     * less the digits. A serial too long for even one base character has no
     * alias, and the caller refuses. */
    uint32_t digits = 1;
    for (uint32_t n = serial; n >= 10; n /= 10) {
        ++digits;
    }
    if (digits + 1 >= 8) {
        return false;
    }
    uint32_t const base_max = 8 - 1 - digits;
    /* Split the name as 8.3 does: the extension follows the last dot that is
     * neither first nor last. */
    uint32_t dot = length;
    for (uint32_t i = 0; i < length; ++i) {
        if (name[i] == '.') {
            dot = i;
        }
    }
    uint32_t const base_end = (dot > 0 && dot + 1 < length) ? dot : length;
    uint32_t const ext_at = base_end < length ? base_end + 1 : length;

    for (uint32_t i = 0; i < 11; ++i) {
        out[i] = ' ';
    }
    uint32_t at = 0;
    for (uint32_t i = 0; i < base_end && at < base_max; ++i) {
        out[at++] = static_cast<uint8_t>(alias_char(static_cast<uint8_t>(name[i])));
    }
    if (at == 0) {
        out[at++] = '_'; /* a name of only dots still needs a body */
    }
    out[at++] = '~';
    for (uint32_t n = serial, divisor = 0; n > 0; n /= 10, ++divisor) {
        out[at + digits - 1 - divisor] = static_cast<uint8_t>('0' + (n % 10));
    }
    at += digits;
    for (uint32_t i = 0; i < 3 && ext_at + i < length; ++i) {
        out[8 + i] = static_cast<uint8_t>(alias_char(static_cast<uint8_t>(name[ext_at + i])));
    }
    return true;
}

void dirent_set_case(uint8_t slot[32], bool base_lower, bool ext_lower) noexcept
{
    slot[12] = static_cast<uint8_t>((base_lower ? 0x08 : 0) | (ext_lower ? 0x10 : 0));
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

namespace {

/* Howard Hinnant's civil-date algorithms: the days since 1970-01-01 for a
 * Gregorian date, and the reverse. FAT's epoch is 1980 and it carries no time
 * zone, so the DOS words are read and written as UTC. */
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) noexcept
{
    y -= m <= 2;
    int64_t const era = (y >= 0 ? y : y - 399) / 400;
    unsigned const yoe = static_cast<unsigned>(y - era * 400);
    unsigned const doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) noexcept
{
    z += 719468;
    int64_t const era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned const doe = static_cast<unsigned>(z - era * 146097);
    unsigned const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t const year = static_cast<int64_t>(yoe) + era * 400;
    unsigned const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned const mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = year + (*m <= 2 ? 1 : 0);
}

}  // namespace

void dirent_set_time(uint8_t slot[32], uint16_t date, uint16_t time) noexcept
{
    put16(slot + kDirentCreateTime, time);
    put16(slot + kDirentCreateDate, date);
    put16(slot + kDirentAccessDate, date);
    put16(slot + kDirentWriteTime, time);
    put16(slot + kDirentWriteDate, date);
}

void dirent_time(uint8_t const *slot, uint16_t *date, uint16_t *time) noexcept
{
    *date = word16(slot + kDirentWriteDate);
    *time = word16(slot + kDirentWriteTime);
}

uint64_t dos_to_unix(uint16_t date, uint16_t time) noexcept
{
    if (date == 0) {
        return 0; /* no time */
    }
    uint32_t const year = 1980u + ((date >> 9) & 0x7fu);
    uint32_t const month = (date >> 5) & 0x0fu;
    uint32_t const day = date & 0x1fu;
    uint32_t const hour = (time >> 11) & 0x1fu;
    uint32_t const minute = (time >> 5) & 0x3fu;
    uint32_t const second = (time & 0x1fu) * 2u;
    int64_t const days = days_from_civil(year, month, day);
    return static_cast<uint64_t>(days) * 86400u + hour * 3600u + minute * 60u + second;
}

void unix_to_dos(uint64_t seconds, uint16_t *date, uint16_t *time) noexcept
{
    if (seconds == 0) {
        *date = 0;
        *time = 0;
        return;
    }
    int64_t const days = static_cast<int64_t>(seconds / 86400u);
    uint32_t const rem = static_cast<uint32_t>(seconds % 86400u);
    int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, &year, &month, &day);
    if (year < 1980) {
        *date = 0;
        *time = 0;
        return;
    }
    *date = static_cast<uint16_t>(((year - 1980) << 9) | (month << 5) | day);
    *time = static_cast<uint16_t>(((rem / 3600) << 11) | (((rem % 3600) / 60) << 5) |
                                  ((rem % 60) / 2));
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
