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
 * It proves the attachment out loud (parse the BPB, list the root directory,
 * read the file the disk was made with back), announces the volume's label
 * on the partition manager's own port -- the manager registers it with the
 * VFS, because a service not in the manifest is given its ports rather than
 * declaring them (specs/vfs.md) -- and then serves the volume protocol on
 * "vol": the root directory, version one, with directories the recorded next
 * step.
 */

#include "fat.h"

#include <aegir/block.h>
#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/descriptor.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/nmspace.h>
#include <aegir/partman.h>
#include <aegir/volume.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

constexpr uint32_t kSectorBytes = 512;

/* What the serve phase works through: the block port, the shared window the
 * reads land in, the volume's own geometry, and the range grant's offset.
 * Static because a spawned process's stack is two pages (specs/userland.md)
 * and the answer buffers below are not small. */
aegir::ipc::Consumer g_blk;
uint8_t *g_window = nullptr;
uint64_t g_first = 0;
uint32_t g_window_sectors = 0;
aegir::fat::Volume g_volume{};

/* One read, volume-relative: the data lands in the window, which every parse
 * then reads. False is a report, not a hang. A read larger than the window
 * refuses rather than chunking, and that is a bound the FAT format itself
 * already keeps: every structure walked here is one sector or one cluster,
 * and the specification caps a cluster at 64 KiB -- exactly this window's
 * size. */
bool read(uint64_t lba, uint32_t count) noexcept
{
    if (count > g_window_sectors) {
        return false;
    }
    aegir::ipc::Reply const reply =
        g_blk.call(aegir::block::kMethodRead, aegir::block::pack_read(g_first + lba, count));
    return reply.error == 0 && reply.word == count;
}

char upper(char c) noexcept
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c;
}

/* FAT names are case-insensitive; a client that asks for "aegir.txt" means
 * "AEGIR.TXT". */
bool same_name(char const *a, uint32_t a_length, char const *b, uint32_t b_length) noexcept
{
    if (a_length != b_length) {
        return false;
    }
    for (uint32_t i = 0; i < a_length; ++i) {
        if (upper(a[i]) != upper(b[i])) {
            return false;
        }
    }
    return true;
}

/* Walk the root directory -- a fixed region on FAT16, a cluster chain like
 * any other on FAT32 -- for one entry: by name when `name` is given, else
 * the index-th entry the directory holds. False is not-found, which a
 * protocol answer reports by saying nothing. */
bool find_in_root(char const *name, uint32_t name_length, uint32_t index,
                  aegir::fat::Dirent *out) noexcept
{
    uint32_t seen = 0;
    bool found = false;
    auto consider = [&](uint32_t entry_count) -> bool {
        for (uint32_t i = 0; i < entry_count; ++i) {
            aegir::fat::Dirent dirent;
            aegir::fat::Entry const kind = aegir::fat::dirent(g_window + i * 32, &dirent);
            if (kind == aegir::fat::Entry::End) {
                return true;
            }
            if (kind == aegir::fat::Entry::Skip) {
                continue;
            }
            if (name != nullptr) {
                if (same_name(dirent.name, dirent.name_length, name, name_length)) {
                    *out = dirent;
                    found = true;
                    return true;
                }
            } else if (seen++ == index) {
                *out = dirent;
                found = true;
                return true;
            }
        }
        return false;
    };
    if (g_volume.fat32) {
        uint32_t cluster = g_volume.root_cluster;
        while (!found && cluster < aegir::fat::kEoc32) {
            if (!read(aegir::fat::cluster_sector(g_volume, cluster),
                      g_volume.sectors_per_cluster)) {
                return false;
            }
            if (consider(g_volume.sectors_per_cluster * kSectorBytes / 32)) {
                break;
            }
            uint32_t const fat_offset = cluster * 4;
            if (!read(g_volume.fat_start + fat_offset / kSectorBytes, 1)) {
                return false;
            }
            cluster = aegir::fat::next32(g_window, cluster % (kSectorBytes / 4));
        }
    } else {
        for (uint32_t s = 0; s < g_volume.root_sectors && !found; ++s) {
            if (!read(g_volume.root_start + s, 1)) {
                return false;
            }
            if (consider(kSectorBytes / 32)) {
                break;
            }
        }
    }
    return found;
}

void answer_read(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                       &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint32_t const path_words = 1 + (path_length + 7) / 8;
    /* The root only, version one: the empty rest names the directory, and a
     * '/' is a path this version does not know. */
    bool nested = path_length == 0;
    for (uint32_t i = 0; i < path_length; ++i) {
        if (path[i] == '/') {
            nested = true;
        }
    }
    if (nested || count < path_words + 2) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const offset = words[path_words];
    uint64_t wanted = words[path_words + 1];
    aegir::fat::Dirent dirent;
    if (!find_in_root(path, path_length, 0, &dirent) || dirent.directory ||
        offset > dirent.bytes) {
        port.reply_words(nullptr, 0);
        return;
    }
    if (wanted > aegir::volume::kReadMax) {
        wanted = aegir::volume::kReadMax;
    }
    uint64_t const available = dirent.bytes - offset;
    uint64_t const got = wanted < available ? wanted : available;
    uint64_t answer[aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8];
    char *bytes = reinterpret_cast<char *>(answer + aegir::volume::kReadHeaderWords);
    /* Follow the chain, copying the overlapping part of each cluster out
     * before the next read -- the FAT step's included -- clobbers the
     * window: the "content belongs to the most recent call" caveat never
     * reaches the volume's clients (aegir/volume.h). */
    uint32_t const cluster_bytes = g_volume.sectors_per_cluster * kSectorBytes;
    uint32_t const eoc = g_volume.fat32 ? aegir::fat::kEoc32 : aegir::fat::kEoc16;
    uint32_t cluster = dirent.first_cluster;
    uint64_t position = 0;
    uint64_t filled = 0;
    while (filled < got && cluster >= 2 && cluster < eoc) {
        if (position + cluster_bytes > offset) {
            if (!read(aegir::fat::cluster_sector(g_volume, cluster),
                      g_volume.sectors_per_cluster)) {
                break;
            }
            uint64_t const at = offset > position ? offset - position : 0;
            uint64_t const room = cluster_bytes - at;
            uint64_t const take = got - filled < room ? got - filled : room;
            for (uint64_t i = 0; i < take; ++i) {
                bytes[filled + i] = g_window[at + i];
            }
            filled += take;
        }
        position += cluster_bytes;
        uint32_t const fat_offset = cluster * (g_volume.fat32 ? 4u : 2u);
        if (!read(g_volume.fat_start + fat_offset / kSectorBytes, 1)) {
            break;
        }
        cluster = g_volume.fat32
                      ? aegir::fat::next32(g_window, cluster % (kSectorBytes / 4))
                      : aegir::fat::next16(g_window, cluster % (kSectorBytes / 2));
    }
    answer[0] = filled;
    answer[1] = offset + filled >= dirent.bytes ? 1 : 0;
    port.reply_words(answer, aegir::volume::kReadHeaderWords +
                                 static_cast<uint32_t>((filled + 7) / 8));
}

void answer_list(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                       &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    /* The root only, version one: the empty rest is the one directory. */
    if (path_length != 0 || count < 2) {
        port.reply_words(nullptr, 0);
        return;
    }
    aegir::fat::Dirent dirent;
    if (!find_in_root(nullptr, 0, static_cast<uint32_t>(words[1]), &dirent)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t answer[aegir::ipc::kMaxWords];
    uint32_t const name_words = aegir::nmspace::pack_string(
        answer, dirent.name, dirent.name_length, aegir::ipc::kMaxWords * 8 -
                                                     aegir::volume::kListTailWords * 8);
    if (name_words == 0 ||
        name_words + aegir::volume::kListTailWords > aegir::ipc::kMaxWords) {
        port.reply_words(nullptr, 0);
        return;
    }
    answer[name_words] = dirent.bytes;
    answer[name_words + 1] = dirent.directory ? aegir::volume::kKindDir
                                              : aegir::volume::kKindFile;
    port.reply_words(answer, name_words + aegir::volume::kListTailWords);
}

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
    char const *descriptor_name = nullptr;
    uint32_t descriptor_name_length = 0;
    while (rows.next_row()) {
        aegir::descriptor::Field field;
        while (rows.next_field(field)) {
            bool ok = false;
            if (aegir::descriptor::key_is(field, "first")) {
                first = aegir::descriptor::number(field, &ok);
            } else if (aegir::descriptor::key_is(field, "sectors")) {
                range_sectors = aegir::descriptor::number(field, &ok);
            } else if (aegir::descriptor::key_is(field, "name")) {
                descriptor_name = field.value;
                descriptor_name_length = field.value_length;
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
    /* The serve phase's half of the attachment, set once here. */
    g_blk = blk;
    g_window = window;
    g_first = first;
    g_window_sectors = window_sectors;

    if (instance != nullptr) {
        aegir::debug_write("      ");
        aegir::debug_write(instance, instance_length);
        aegir::debug_write(": sectors ");
        aegir::debug_write_unsigned(first);
        aegir::debug_write("..");
        aegir::debug_write_unsigned(first + range_sectors - 1);
        aegir::debug_write(" of the device\n");
    }

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
    g_volume = volume;
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

    /* The volume's public name: the label in its own boot sector when it
     * has one -- 11 space-padded bytes where the flavor keeps it -- the
     * descriptor's name otherwise, and the instance's own name as the last
     * word. The boot sector is re-read because the demo's walks have
     * clobbered the window since. */
    char label[11];
    uint32_t label_length = 0;
    if (read(0, 1)) {
        uint32_t const at = volume.fat32 ? 71 : 43;
        for (uint32_t i = 0; i < sizeof(label); ++i) {
            label[i] = window[at + i];
        }
        label_length = sizeof(label);
        while (label_length > 0 &&
               (label[label_length - 1] == ' ' || label[label_length - 1] == '\0')) {
            --label_length;
        }
    }
    char const *volume_name = label;
    uint32_t volume_name_length = label_length;
    if (volume_name_length == 0) {
        volume_name = descriptor_name;
        volume_name_length = descriptor_name_length;
    }
    if (volume_name_length == 0 && instance_length > 4) {
        volume_name = instance + 4; /* the name without the kind */
        volume_name_length = instance_length - 4;
    }

    /* Announce, then serve: the partition manager is waiting for the one
     * call, and its answer is the name the volume actually got from the
     * VFS. An empty answer is a refusal -- the volume serves anyway,
     * under no name. */
    aegir::ipc::Owner vol = aegir::ipc::Owner::find("vol", 3);
    aegir::ipc::Consumer const partman = aegir::ipc::Consumer::find(
        aegir::partman::kPortName, aegir::partman::kPortNameLength);
    if (!vol.valid()) {
        aegir::debug_write("      FAIL fs.fat: no volume port\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    if (partman.valid() && volume_name_length != 0) {
        uint64_t out[aegir::nmspace::kNameMax / 8 + 1];
        uint32_t const out_words = aegir::nmspace::pack_string(
            out, volume_name, volume_name_length, aegir::nmspace::kNameMax);
        uint64_t in[aegir::nmspace::kNameMax / 8 + 1];
        aegir::ipc::WordsReply const announced =
            partman.call_words(aegir::partman::kMethodAnnounce, out, out_words, in,
                               aegir::nmspace::kNameMax / 8 + 1);
        char const *assigned = nullptr;
        uint32_t assigned_length = 0;
        aegir::debug_write("      ");
        aegir::debug_write(instance, instance_length);
        aegir::debug_write(": ");
        if (announced.error != 0 || announced.count == 0 ||
            !aegir::nmspace::unpack_string(in, announced.count, aegir::nmspace::kNameMax,
                                           &assigned, &assigned_length)) {
            aegir::debug_write("the announce was refused\n");
        } else {
            aegir::debug_write(assigned, assigned_length);
            aegir::debug_write(": announced, serving\n");
        }
    }

    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        uint32_t const method =
            vol.receive_words(words, aegir::ipc::kMaxWords, &count, nullptr);
        switch (method) {
        case aegir::volume::kMethodRead:
            answer_read(vol, words, count);
            break;
        case aegir::volume::kMethodList:
            answer_list(vol, words, count);
            break;
        default:
            /* A method this version does not know is answered by saying
             * nothing (specs/services.md's versioning rule). */
            vol.reply_words(nullptr, 0);
            break;
        }
    }
}
