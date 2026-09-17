/*
 * aegir-fs-fat: the FAT filesystem service.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One instance per FAT partition, started by the partition manager with the
 * block device's port and a range grant (offset and length) rather than the
 * whole device (specs/services.md). FAT16/32/ExFAT are the interchange
 * filesystems -- how Aegir exchanges data with the rest of the world, not its
 * own filesystem -- and read-only to begin with.
 *
 * Today it proves the attachment: parse the BPB, list the root directory,
 * and read the file the disk was made with back out loud.
 */

#include "fat.h"

#include <aegir/block.h>
#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/descriptor.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

constexpr uint32_t kSectorBytes = 512;

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    uint32_t instance_length = 0;
    char const *instance = aegir::bootstrap::name(&instance_length);

    aegir::ipc::Consumer const blk = aegir::ipc::Consumer::find("blk", 3);
    uint64_t window_address = 0;
    uint32_t window_bytes = 0;
    uint64_t window_physical = 0;
    if (!blk.valid() ||
        !aegir::bootstrap::shared_window(&window_address, &window_bytes,
                                         &window_physical)) {
        aegir::debug_write("      FAIL fs.fat: no block port or no shared window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The range grant, as the descriptor row the partition manager wrote:
     * where this volume starts on the device and how far it runs. Every read
     * below is volume-relative; the offset is added here and nowhere else. */
    uint64_t blob_address = 0;
    uint32_t blob_bytes = 0;
    uint64_t first = 0;
    uint64_t range_sectors = 0;
    if (!aegir::bootstrap::devices(&blob_address, &blob_bytes) || blob_bytes == 0) {
        aegir::debug_write("      FAIL fs.fat: no range grant\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    aegir::descriptor::Reader rows(reinterpret_cast<void const *>(blob_address),
                                   blob_bytes);
    while (rows.next_row()) {
        aegir::descriptor::Field field;
        while (rows.next_field(field)) {
            bool ok = false;
            if (aegir::descriptor::key_is(field, "first")) {
                first = aegir::descriptor::number(field, &ok);
            } else if (aegir::descriptor::key_is(field, "sectors")) {
                range_sectors = aegir::descriptor::number(field, &ok);
            }
        }
    }
    if (first == 0 || range_sectors == 0) {
        aegir::debug_write("      FAIL fs.fat: the range grant is empty\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    auto *window = reinterpret_cast<uint8_t *>(window_address);
    uint32_t const window_sectors = window_bytes / kSectorBytes;

    if (instance != nullptr) {
        aegir::debug_write("      ");
        aegir::debug_write(instance, instance_length);
        aegir::debug_write(": sectors ");
        aegir::debug_write_unsigned(first);
        aegir::debug_write("..");
        aegir::debug_write_unsigned(first + range_sectors - 1);
        aegir::debug_write(" of the device\n");
    }

    /* One read, volume-relative: the data lands in the window, which every
     * parse below then reads. False is a report, not a hang. A read larger
     * than the window refuses rather than chunking, and that is a bound the
     * FAT format itself already keeps: every structure walked here is one
     * sector or one cluster, and the specification caps a cluster at 64 KiB
     * -- exactly this window's size. */
    auto read = [&blk, window, first, window_sectors](uint64_t lba, uint32_t count) -> bool {
        if (count > window_sectors) {
            return false;
        }
        aegir::ipc::Reply const reply =
            blk.call(aegir::block::kMethodRead, aegir::block::pack_read(first + lba, count));
        return reply.error == 0 && reply.word == count;
    };

    if (!read(0, 1)) {
        aegir::debug_write("      FAIL fs.fat: the BPB would not read\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    aegir::fat::Volume volume;
    if (!aegir::fat::bpb(window, &volume)) {
        aegir::debug_write("      FAIL fs.fat: sector 0 is not a FAT BPB\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    aegir::debug_write("      ");
    aegir::debug_write(instance, instance_length);
    aegir::debug_write(": FAT");
    aegir::debug_write(volume.fat32 ? "32" : "16");
    aegir::debug_write(", ");
    aegir::debug_write_unsigned(volume.sectors_per_cluster);
    aegir::debug_write(" sectors per cluster, data starts at sector ");
    aegir::debug_write_unsigned(volume.data_start);
    aegir::debug_write("\n");

    /* The root directory: a fixed region on FAT16, a cluster chain like any
     * other on FAT32. The first plain file with content is remembered, to be
     * read back below. */
    aegir::fat::Dirent target{};
    bool have_target = false;
    auto list_entries = [&](uint32_t entry_count) {
        for (uint32_t i = 0; i < entry_count; ++i) {
            aegir::fat::Dirent dirent;
            aegir::fat::Entry const kind = aegir::fat::dirent(window + i * 32, &dirent);
            if (kind == aegir::fat::Entry::End) {
                return true;
            }
            if (kind == aegir::fat::Entry::Skip || dirent.directory) {
                continue;
            }
            aegir::debug_write("      ");
            aegir::debug_write(instance, instance_length);
            aegir::debug_write(": ");
            aegir::debug_write(dirent.name, dirent.name_length);
            aegir::debug_write(", ");
            aegir::debug_write_unsigned(dirent.bytes);
            aegir::debug_write(" bytes\n");
            if (!have_target && dirent.bytes > 0) {
                target = dirent;
                have_target = true;
            }
        }
        return false;
    };

    bool walked = true;
    if (volume.fat32) {
        uint32_t cluster = volume.root_cluster;
        while (cluster < aegir::fat::kEoc32) {
            if (!read(aegir::fat::cluster_sector(volume, cluster),
                      volume.sectors_per_cluster)) {
                walked = false;
                break;
            }
            if (list_entries(volume.sectors_per_cluster * kSectorBytes / 32)) {
                break;
            }
            uint32_t const fat_offset = cluster * 4;
            if (!read(volume.fat_start + fat_offset / kSectorBytes, 1)) {
                walked = false;
                break;
            }
            cluster = aegir::fat::next32(window, cluster % (kSectorBytes / 4));
        }
    } else {
        for (uint32_t s = 0; s < volume.root_sectors && walked; ++s) {
            if (!read(volume.root_start + s, 1)) {
                walked = false;
                break;
            }
            if (list_entries(kSectorBytes / 32)) {
                break;
            }
        }
    }
    if (!walked) {
        aegir::debug_write("      FAIL fs.fat: the root directory would not read\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* Read the file back: follow its chain and print what it says. The file
     * the disk was made with is the checksum -- a reader that walked the
     * wrong sectors does not print this. */
    if (have_target) {
        uint32_t cluster = target.first_cluster;
        uint32_t left = target.bytes;
        aegir::debug_write("      ");
        aegir::debug_write(instance, instance_length);
        aegir::debug_write(": ");
        aegir::debug_write(target.name, target.name_length);
        aegir::debug_write(" says: ");
        /* The chain step is the one place the flavors differ: 4-byte entries
         * on FAT32, 2-byte on FAT16, and each with its own end-of-chain
         * floor. */
        uint32_t const eoc = volume.fat32 ? aegir::fat::kEoc32 : aegir::fat::kEoc16;
        while (left > 0 && cluster >= 2 && cluster < eoc) {
            if (!read(aegir::fat::cluster_sector(volume, cluster),
                      volume.sectors_per_cluster)) {
                aegir::debug_write("(a cluster would not read)");
                left = 0;
                break;
            }
            uint32_t const here = volume.sectors_per_cluster * kSectorBytes;
            uint32_t const shown = left < here ? left : here;
            aegir::debug_write(reinterpret_cast<char const *>(window), shown);
            left -= shown;
            uint32_t const fat_offset = cluster * (volume.fat32 ? 4u : 2u);
            if (!read(volume.fat_start + fat_offset / kSectorBytes, 1)) {
                break;
            }
            cluster = volume.fat32
                          ? aegir::fat::next32(window, cluster % (kSectorBytes / 4))
                          : aegir::fat::next16(window, cluster % (kSectorBytes / 2));
        }
        aegir::debug_write("\n");
    }

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
