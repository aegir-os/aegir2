/*
 * aegir-fs-bfs: the Be File System service.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One instance per BFS partition, started by the partition manager with a
 * block device's port and a range grant (specs/services.md). It reads the
 * volume through libs/aegir-bfs and serves the volume protocol's read side
 * (specs/vfs.md); writes are the growth phase, so the write methods refuse.
 *
 * The bootstrap is the FAT service's shape, because the partition manager
 * treats every filesystem alike: the block port and a range descriptor, the
 * volume port, the announce, and the optional clock.
 */

#include <aegir/block.h>
#include <aegir/bootstrap.h>
#include <aegir/clock.h>
#include <aegir/debug.h>
#include <aegir/descriptor.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/nmspace.h>
#include <aegir/partman.h>
#include <aegir/volume.h>
#include <aegir/bfs/volume.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

constexpr uint32_t kSectorBytes = 512;
constexpr uint32_t kReadMax = aegir::volume::kReadMax;

/* The volume, and the transport it reads through. The Volume holds its own
 * block buffers (tens of kilobytes), so it is not a local. */
aegir::bfs::Volume g_volume;
aegir::ipc::Consumer g_blk;
uint8_t *g_window = nullptr;
uint64_t g_first = 0;
bool g_writable = false;

/* Reading for the volume: a volume-relative 512-byte sector, through the
 * block port and into the window this service was given, copied out before
 * the next call clobbers it (aegir/block.h). */
bool read_sector(void *context, uint64_t sector, uint8_t *out) noexcept
{
    static_cast<void>(context);
    if (g_window == nullptr) {
        return false;
    }
    aegir::ipc::Reply const reply =
        g_blk.call(aegir::block::kMethodRead, aegir::block::pack_read(g_first + sector, 1));
    if (reply.error != 0 || reply.word != 1) {
        return false;
    }
    for (uint32_t i = 0; i < kSectorBytes; ++i) {
        out[i] = g_window[i];
    }
    return true;
}

bool is_directory(aegir::bfs::Inode const &inode) noexcept
{
    return (inode.mode & 0xf000) == 0x4000;
}

/* Walk a component path from the root. The empty path is the root; an empty
 * component and ".." are the parent, "." the directory itself (specs/vfs.md's
 * Amiga convention). */
bool walk(char const *path, uint32_t length, aegir::bfs::Inode *out) noexcept
{
    aegir::bfs::Inode inode;
    if (!g_volume.read_inode(g_volume.root_block(), &inode)) {
        return false;
    }
    uint32_t start = 0;
    while (start < length) {
        uint32_t end = start;
        while (end < length && path[end] != '/') {
            ++end;
        }
        uint32_t const component = end - start;
        if (component == 0) {
            if (!g_volume.read_inode(g_volume.to_block(inode.parent), &inode)) {
                return false;
            }
        } else if (component == 1 && path[start] == '.') {
            /* the directory itself: nothing to do */
        } else if (component == 2 && path[start] == '.' && path[start + 1] == '.') {
            if (!g_volume.read_inode(g_volume.to_block(inode.parent), &inode)) {
                return false;
            }
        } else {
            uint64_t child = 0;
            if (!g_volume.dir_find(inode, path + start, component, &child) ||
                !g_volume.read_inode(child, &inode)) {
                return false;
            }
        }
        start = end + 1;
    }
    *out = inode;
    return true;
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
    if (count < path_words + 2) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const offset = words[path_words];
    uint64_t wanted = words[path_words + 1];
    aegir::bfs::Inode inode;
    if (!walk(path, path_length, &inode) || is_directory(inode) ||
        offset > static_cast<uint64_t>(inode.size)) {
        port.reply_words(nullptr, 0);
        return;
    }
    if (wanted > kReadMax) {
        wanted = kReadMax;
    }
    uint64_t const available = static_cast<uint64_t>(inode.size) - offset;
    uint32_t const got = static_cast<uint32_t>(wanted < available ? wanted : available);
    uint64_t answer[aegir::volume::kReadHeaderWords + kReadMax / 8];
    auto *bytes = reinterpret_cast<uint8_t *>(answer + aegir::volume::kReadHeaderWords);
    if (!g_volume.read_stream(inode, offset, bytes, got)) {
        port.reply_words(nullptr, 0);
        return;
    }
    answer[0] = got;
    answer[1] = offset + got >= static_cast<uint64_t>(inode.size) ? 1 : 0;
    port.reply_words(answer,
                     aegir::volume::kReadHeaderWords + static_cast<uint32_t>((got + 7) / 8));
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
    uint32_t const path_words = 1 + (path_length + 7) / 8;
    if (count < path_words + 1) {
        port.reply_words(nullptr, 0);
        return;
    }
    aegir::bfs::Inode dir;
    if (!walk(path, path_length, &dir) || !is_directory(dir)) {
        port.reply_words(nullptr, 0);
        return;
    }
    char name[aegir::bfs::kMaxName];
    uint32_t name_length = 0;
    uint64_t block = 0;
    if (!g_volume.dir_entry(dir, static_cast<uint32_t>(words[path_words]), name,
                            &name_length, &block)) {
        port.reply_words(nullptr, 0);
        return;
    }
    aegir::bfs::Inode child;
    if (!g_volume.read_inode(block, &child)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t answer[aegir::ipc::kMaxWords];
    uint32_t const name_words = aegir::nmspace::pack_string(
        answer, name, name_length,
        aegir::ipc::kMaxWords * 8 - aegir::volume::kListTailWords * 8);
    if (name_words == 0 ||
        name_words + aegir::volume::kListTailWords > aegir::ipc::kMaxWords) {
        port.reply_words(nullptr, 0);
        return;
    }
    answer[name_words] = is_directory(child) ? 0 : static_cast<uint64_t>(child.size);
    answer[name_words + 1] =
        is_directory(child) ? aegir::volume::kKindDir : aegir::volume::kKindFile;
    port.reply_words(answer, name_words + aegir::volume::kListTailWords);
}

void answer_stat(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                       &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    aegir::bfs::Inode inode;
    if (!walk(path, path_length, &inode)) {
        port.reply_words(nullptr, 0);
        return;
    }
    bool const directory = is_directory(inode);
    uint64_t const answer[aegir::volume::kStatTailWords] = {
        directory ? aegir::volume::kKindDir : aegir::volume::kKindFile,
        directory ? 0 : static_cast<uint64_t>(inode.size),
        static_cast<uint64_t>(inode.mtime) >> 16,
    };
    port.reply_words(answer, aegir::volume::kStatTailWords);
}

void answer_refuse(aegir::ipc::Owner &port) noexcept
{
    /* The write side is the growth phase: a refusal, not a lie. */
    port.reply_words(nullptr, 0);
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
    if (!blk.valid() || !aegir::bootstrap::shared_window(&window_address, &window_bytes,
                                                         &window_physical)) {
        aegir::debug_write("      FAIL fs.bfs: no block port or no shared window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    uint64_t blob_address = 0;
    uint32_t blob_bytes = 0;
    uint64_t first = 0;
    uint64_t range_sectors = 0;
    if (!aegir::bootstrap::devices(&blob_address, &blob_bytes) || blob_bytes == 0) {
        aegir::debug_write("      FAIL fs.bfs: no range grant\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    aegir::descriptor::Reader rows(reinterpret_cast<void const *>(blob_address), blob_bytes);
    while (rows.next_row()) {
        aegir::descriptor::Field field;
        while (rows.next_field(field)) {
            bool ok = false;
            if (aegir::descriptor::key_is(field, "first")) {
                first = aegir::descriptor::number(field, &ok);
            } else if (aegir::descriptor::key_is(field, "sectors")) {
                range_sectors = aegir::descriptor::number(field, &ok);
            } else if (aegir::descriptor::key_is(field, "writable")) {
                g_writable = aegir::descriptor::number(field, &ok) != 0;
            }
        }
    }
    if (first == 0 || range_sectors == 0) {
        aegir::debug_write("      FAIL fs.bfs: the range grant is empty\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    static_cast<void>(window_bytes);
    static_cast<void>(window_physical);
    g_blk = blk;
    g_window = reinterpret_cast<uint8_t *>(window_address);
    g_first = first;

    if (instance != nullptr) {
        aegir::debug_write("      ");
        aegir::debug_write(instance, instance_length);
        aegir::debug_write(": sectors ");
        aegir::debug_write_unsigned(first);
        aegir::debug_write("..");
        aegir::debug_write_unsigned(first + range_sectors - 1);
        aegir::debug_write(" of the device\n");
    }

    if (!g_volume.open(read_sector, nullptr)) {
        aegir::debug_write("      FAIL fs.bfs: not a Be File System this reader speaks\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The volume name: the superblock's, NUL-trimmed. */
    char label[32];
    uint32_t label_length = 0;
    for (uint32_t i = 0; i < sizeof(label) && g_volume.name()[i] != '\0'; ++i) {
        label[i] = g_volume.name()[i];
        label_length = i + 1;
    }

    aegir::debug_write("      ");
    aegir::debug_write(instance, instance_length);
    aegir::debug_write(": BeFS, block size ");
    aegir::debug_write_unsigned(g_volume.block_size());
    aegir::debug_write(", ");
    aegir::debug_write_unsigned(g_volume.num_blocks());
    aegir::debug_write(" blocks");
    if (g_writable) {
        aegir::debug_write(", writable");
    }
    aegir::debug_write("\n");

    /* Read the root directory back: the entries are the proof the walk and
     * the trees are read right. */
    aegir::bfs::Inode root;
    if (g_volume.read_inode(g_volume.root_block(), &root)) {
        for (uint32_t index = 0;; ++index) {
            char name[aegir::bfs::kMaxName];
            uint32_t name_length = 0;
            uint64_t block = 0;
            if (!g_volume.dir_entry(root, index, name, &name_length, &block)) {
                break;
            }
            aegir::bfs::Inode child;
            if (!g_volume.read_inode(block, &child)) {
                break;
            }
            aegir::debug_write("      ");
            aegir::debug_write(instance, instance_length);
            aegir::debug_write(": ");
            aegir::debug_write(name, name_length);
            if (is_directory(child)) {
                aegir::debug_write(" (directory)\n");
            } else {
                aegir::debug_write(", ");
                aegir::debug_write_unsigned(static_cast<uint64_t>(child.size));
                aegir::debug_write(" bytes\n");
            }
        }
    }

    aegir::ipc::Owner vol = aegir::ipc::Owner::find("vol", 3);
    aegir::ipc::Consumer const partman = aegir::ipc::Consumer::find(
        aegir::partman::kPortName, aegir::partman::kPortNameLength);
    if (!vol.valid()) {
        aegir::debug_write("      FAIL fs.bfs: no volume port\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    if (partman.valid() && label_length != 0) {
        uint64_t out[aegir::nmspace::kNameMax / 8 + 1];
        uint32_t const out_words = aegir::nmspace::pack_string(out, label, label_length,
                                                               aegir::nmspace::kNameMax);
        uint64_t in[aegir::nmspace::kNameMax / 8 + 1];
        aegir::ipc::WordsReply const announced = partman.call_words(
            aegir::partman::kMethodAnnounce, out, out_words, in,
            aegir::nmspace::kNameMax / 8 + 1);
        aegir::debug_write("      ");
        aegir::debug_write(instance, instance_length);
        aegir::debug_write(": ");
        char const *assigned = nullptr;
        uint32_t assigned_length = 0;
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
        seL4_Word badge = 0;
        uint32_t const method =
            vol.receive_words(words, aegir::ipc::kMaxWords, &count, &badge);
        switch (method) {
        case aegir::volume::kMethodRead:
            answer_read(vol, words, count);
            break;
        case aegir::volume::kMethodList:
            answer_list(vol, words, count);
            break;
        case aegir::volume::kMethodStat:
            answer_stat(vol, words, count);
            break;
        default:
            /* The write side, the handle side, attributes and queries: the
             * growth phases, refused rather than answered wrongly. */
            answer_refuse(vol);
            break;
        }
    }
}
