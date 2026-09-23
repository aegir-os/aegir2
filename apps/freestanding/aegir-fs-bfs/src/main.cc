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
#include <aegir/metadata.h>
#include <aegir/nmspace.h>
#include <aegir/partman.h>
#include <aegir/volume.h>
#include <aegir/bfs/query.h>
#include <aegir/bfs/volume.h>
#include <aegir/bfs/writer.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

constexpr uint32_t kSectorBytes = 512;
constexpr uint32_t kReadMax = aegir::volume::kReadMax;

/* The volume, and the transport it reads through. The Volume holds its own
 * block buffers (tens of kilobytes), so it is not a local. */
aegir::bfs::Volume g_volume;
aegir::bfs::Writer g_writer;
/* One parsed query, reused: the service answers one call at a time, and a
 * query is re-parsed from its text on each step rather than held. */
aegir::bfs::Query g_query;
aegir::ipc::Consumer g_blk;
uint8_t *g_window = nullptr;
uint64_t g_first = 0;
bool g_writable = false;

/* The handle table's page and its serial, exactly as fs-fat keeps them. */
uint8_t *g_memory = nullptr;
uint32_t g_memory_bytes = 0;
uint64_t g_handle_serial = 0;

aegir::ipc::Consumer g_clock;
bool g_have_clock = false;

uint64_t now_seconds() noexcept
{
    if (!g_have_clock) {
        return 0;
    }
    uint64_t answer[aegir::clock::kNowWords] = {};
    aegir::ipc::WordsReply const reply = g_clock.call_words(
        aegir::clock::kMethodNow, nullptr, 0, answer, aegir::clock::kNowWords);
    return reply.error == 0 && reply.count >= 1 ? answer[0] : 0;
}

/* The inode time is seconds in the high 48 bits and a 16-bit sub-second field
 * in the low bits; with a seconds clock the low field is zero (specs/bfs.md's
 * Times). */
int64_t inode_time() noexcept
{
    return static_cast<int64_t>(now_seconds()) << 16;
}

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

/* The write's half: the sector goes into the window, then out through the
 * same block port, clamped by this service's badge exactly as a read is. */
bool write_sector(void *context, uint64_t sector, uint8_t const *in) noexcept
{
    static_cast<void>(context);
    if (g_window == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < kSectorBytes; ++i) {
        g_window[i] = in[i];
    }
    aegir::ipc::Reply const reply =
        g_blk.call(aegir::block::kMethodWrite, aegir::block::pack_read(g_first + sector, 1));
    return reply.error == 0 && reply.word == 1;
}

/* The Writer turns each operation into a journal transaction: it writes the
 * metadata to the log, marks the volume 'DIRT' with log_end past the entry,
 * then puts the blocks home and marks it 'CLEN' with log_start == log_end
 * (specs/bfs.md's journal). A clean volume is one Haiku mounts with no
 * replay. */

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

/* Split a path into the directory that holds its last component and the
 * component itself, as a write operation wants them. The empty path has no
 * last component and is refused. */
bool walk_parent(char const *path, uint32_t length, uint64_t *parent_block,
                 char const **name, uint32_t *name_length) noexcept
{
    uint64_t dir_block = g_volume.root_block();
    uint32_t start = 0;
    while (start < length) {
        uint32_t end = start;
        while (end < length && path[end] != '/') {
            ++end;
        }
        uint32_t const component = end - start;
        if (end == length) {
            if (component == 0) {
                return false;
            }
            *parent_block = dir_block;
            *name = path + start;
            *name_length = component;
            return true;
        }
        aegir::bfs::Inode dir;
        if (!g_volume.read_inode(dir_block, &dir)) {
            return false;
        }
        if (component == 0) {
            dir_block = g_volume.to_block(dir.parent);
        } else if (component == 1 && path[start] == '.') {
            /* the directory itself */
        } else if (component == 2 && path[start] == '.' && path[start + 1] == '.') {
            dir_block = g_volume.to_block(dir.parent);
        } else {
            uint64_t child = 0;
            if (!g_volume.dir_find(dir, path + start, component, &child)) {
                return false;
            }
            dir_block = child;
        }
        start = end + 1;
    }
    return false;
}

/* mkdir's shape: make every missing component on the way, leaving the ones
 * that are there alone. An existing component that is a file refuses. */
bool make_dirs(char const *path, uint32_t length) noexcept
{
    uint64_t dir_block = g_volume.root_block();
    uint32_t start = 0;
    while (start < length) {
        uint32_t end = start;
        while (end < length && path[end] != '/') {
            ++end;
        }
        uint32_t const component = end - start;
        if (component != 0) {
            aegir::bfs::Inode dir;
            if (!g_volume.read_inode(dir_block, &dir)) {
                return false;
            }
            uint64_t child = 0;
            if (g_volume.dir_find(dir, path + start, component, &child)) {
                aegir::bfs::Inode made;
                if (!g_volume.read_inode(child, &made) || !is_directory(made)) {
                    return false;
                }
                dir_block = child;
            } else {
                uint64_t created = 0;
                if (!g_writer.create(dir_block, path + start, component,
                                     aegir::bfs::kModeDirectory | 0755, inode_time(),
                                     &created)) {
                    return false;
                }
                dir_block = created;
            }
        }
        start = end + 1;
    }
    return true;
}

/* One open file or query. The serial is the handle the client names; it is
 * never reused. The badge is whose it is. A file's inode block is where the
 * writes land and its cursor is the byte offset; a query's text is what it
 * evaluates and its cursor is the block the scan resumes at. */
struct Handle {
    uint64_t serial;
    uint64_t badge;
    uint64_t inode_block;
    uint64_t cursor;
    uint8_t kind; /* 0 file, 1 query */
    uint32_t query_length;
    /* A query whose single term an index can answer walks the index rather
     * than the volume: `index_block` names the index, `index_position` is how
     * far the walk has got, and 0 means scan instead. */
    uint64_t index_block;
    uint32_t index_position;
    char query[aegir::metadata::kQueryTextMax];
};

Handle *handle_lookup(uint64_t serial, uint64_t badge) noexcept
{
    if (serial == 0 || g_memory == nullptr) {
        return nullptr;
    }
    uint32_t const capacity = g_memory_bytes / static_cast<uint32_t>(sizeof(Handle));
    auto *rows = reinterpret_cast<Handle *>(g_memory);
    for (uint32_t i = 0; i < capacity; ++i) {
        if (rows[i].serial == serial) {
            return rows[i].badge == badge ? rows + i : nullptr;
        }
    }
    return nullptr;
}

Handle *handle_alloc(uint64_t badge) noexcept
{
    if (g_memory == nullptr) {
        return nullptr;
    }
    uint32_t const capacity = g_memory_bytes / static_cast<uint32_t>(sizeof(Handle));
    auto *rows = reinterpret_cast<Handle *>(g_memory);
    for (uint32_t i = 0; i < capacity; ++i) {
        if (rows[i].serial == 0) {
            rows[i].serial = ++g_handle_serial;
            rows[i].badge = badge;
            return rows + i;
        }
    }
    return nullptr;
}

uint64_t handle_reap(uint64_t badge) noexcept
{
    if (g_memory == nullptr) {
        return 0;
    }
    uint32_t const capacity = g_memory_bytes / static_cast<uint32_t>(sizeof(Handle));
    auto *rows = reinterpret_cast<Handle *>(g_memory);
    uint64_t reaped = 0;
    for (uint32_t i = 0; i < capacity; ++i) {
        if (rows[i].serial != 0 && rows[i].badge == badge) {
            rows[i].serial = 0;
            ++reaped;
        }
    }
    return reaped;
}

void answer_open(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                 uint64_t badge) noexcept
{
    uint64_t handle = 0;
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                       &path_length)) {
        port.reply_words(&handle, 1);
        return;
    }
    uint32_t const path_words = 1 + (path_length + 7) / 8;
    if (count < path_words + 1 || !g_writable) {
        port.reply_words(&handle, 1);
        return;
    }
    uint64_t const flags = words[path_words];
    uint64_t parent_block = 0;
    char const *name = nullptr;
    uint32_t name_length = 0;
    if (!walk_parent(path, path_length, &parent_block, &name, &name_length)) {
        port.reply_words(&handle, 1);
        return;
    }
    aegir::bfs::Inode parent;
    if (!g_volume.read_inode(parent_block, &parent)) {
        port.reply_words(&handle, 1);
        return;
    }
    uint64_t inode_block = 0;
    uint64_t cursor = 0;
    bool ok = false;
    if (g_volume.dir_find(parent, name, name_length, &inode_block)) {
        aegir::bfs::Inode existing;
        if (!g_volume.read_inode(inode_block, &existing) || is_directory(existing)) {
            port.reply_words(&handle, 1);
            return;
        }
        /* An existing name without `create` is refused: opening means
         * meaning to remake it, as the flag says. Truncate frees the chain
         * at once; without it the file keeps its bytes. */
        if ((flags & aegir::volume::kOpenCreate) != 0) {
            if ((flags & aegir::volume::kOpenTruncate) != 0) {
                ok = g_writer.truncate(inode_block, 0, inode_time());
            } else {
                ok = true;
            }
        }
    } else if ((flags & aegir::volume::kOpenCreate) != 0) {
        ok = g_writer.create(parent_block, name, name_length,
                             aegir::bfs::kModeRegular | 0644, inode_time(), &inode_block);
    }
    if (ok) {
        Handle *row = handle_alloc(badge);
        if (row != nullptr) {
            row->inode_block = inode_block;
            row->cursor = cursor;
            row->kind = 0;
            row->query_length = 0;
            handle = row->serial;
        }
    }
    port.reply_words(&handle, 1);
}

void answer_write(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                  uint64_t badge) noexcept
{
    uint64_t written = 0;
    if (count < 2) {
        port.reply_words(&written, 1);
        return;
    }
    Handle *handle = handle_lookup(words[0], badge);
    uint64_t const bytes = words[1];
    if (handle == nullptr || bytes > aegir::volume::kWriteMax ||
        count < 2 + static_cast<uint32_t>((bytes + 7) / 8)) {
        port.reply_words(&written, 1);
        return;
    }
    auto const *data = reinterpret_cast<uint8_t const *>(words + 2);
    if (g_writer.write(handle->inode_block, handle->cursor, data,
                       static_cast<uint32_t>(bytes), inode_time())) {
        handle->cursor += bytes;
        written = bytes;
    }
    port.reply_words(&written, 1);
}

void answer_close(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                  uint64_t badge) noexcept
{
    uint64_t closed = 0;
    if (count >= 1) {
        Handle *handle = handle_lookup(words[0], badge);
        if (handle != nullptr) {
            handle->serial = 0;
            closed = 1;
        }
    }
    port.reply_words(&closed, 1);
}

void answer_reap(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    uint64_t reaped = 0;
    if (count >= 1) {
        reaped = handle_reap(words[0]);
    }
    port.reply_words(&reaped, 1);
}

void answer_mkdir(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    uint64_t made = 0;
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (g_writable && count != 0 &&
        aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                      &path_length) &&
        path_length != 0 && make_dirs(path, path_length)) {
        made = 1;
    }
    port.reply_words(&made, 1);
}

void answer_remove(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    uint64_t removed = 0;
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (g_writable && count != 0 &&
        aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                      &path_length)) {
        uint64_t parent_block = 0;
        char const *name = nullptr;
        uint32_t name_length = 0;
        if (walk_parent(path, path_length, &parent_block, &name, &name_length) &&
            g_writer.remove(parent_block, name, name_length)) {
            removed = 1;
        }
    }
    port.reply_words(&removed, 1);
}

void answer_rename(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    uint64_t renamed = 0;
    char const *src = nullptr;
    uint32_t src_length = 0;
    if (g_writable && count != 0 &&
        aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &src,
                                      &src_length)) {
        uint32_t const src_words = 1 + (src_length + 7) / 8;
        char const *dst = nullptr;
        uint32_t dst_length = 0;
        if (count >= src_words + 1 &&
            aegir::nmspace::unpack_string(words + src_words, count - src_words,
                                          aegir::nmspace::kPathMax, &dst,
                                          &dst_length)) {
            uint64_t src_parent = 0;
            uint64_t dst_parent = 0;
            char const *src_name = nullptr;
            char const *dst_name = nullptr;
            uint32_t src_name_length = 0;
            uint32_t dst_name_length = 0;
            if (walk_parent(src, src_length, &src_parent, &src_name, &src_name_length) &&
                walk_parent(dst, dst_length, &dst_parent, &dst_name, &dst_name_length) &&
                src_parent == dst_parent &&
                g_writer.rename(src_parent, src_name, src_name_length, dst_name,
                                dst_name_length)) {
                renamed = 1;
            }
        }
    }
    port.reply_words(&renamed, 1);
}

void answer_truncate(aegir::ipc::Owner &port, uint64_t const *words,
                     uint32_t count) noexcept
{
    uint64_t resized = 0;
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (g_writable && count != 0 &&
        aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                      &path_length)) {
        uint32_t const path_words = 1 + (path_length + 7) / 8;
        if (count >= path_words + 1) {
            aegir::bfs::Inode inode;
            if (walk(path, path_length, &inode) && !is_directory(inode)) {
                uint64_t parent_block = 0;
                char const *name = nullptr;
                uint32_t name_length = 0;
                uint64_t inode_block = 0;
                if (walk_parent(path, path_length, &parent_block, &name, &name_length) &&
                    g_volume.read_inode(parent_block, &inode) &&
                    g_volume.dir_find(inode, name, name_length, &inode_block) &&
                    g_writer.truncate(inode_block, words[path_words], inode_time())) {
                    resized = 1;
                }
            }
        }
    }
    port.reply_words(&resized, 1);
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

/* A query returns named entries: attribute inodes, attribute directories and
 * the index directory are the filesystem's own, not names a client asked
 * about. */
bool is_queryable(aegir::bfs::Inode const &inode) noexcept
{
    if ((inode.mode &
         (aegir::bfs::kModeAttr | aegir::bfs::kModeAttrDir |
          aegir::bfs::kModeIndexDir)) != 0) {
        return false;
    }
    return inode.name_length != 0;
}

bool bytes_match(char const *a, uint32_t a_length, char const *b,
                 uint32_t b_length) noexcept
{
    if (a_length != b_length) {
        return false;
    }
    for (uint32_t i = 0; i < a_length; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

/* The index a query's one equality term can walk, and the key to walk it
 * with. The key is the bytes the Writer keys that index by: a string
 * attribute's value, or an int64's little-endian coding. False when the query
 * is not a lone equality on an indexed attribute. */
struct IndexTerm {
    char const *name;
    uint32_t name_length;
    uint8_t const *key;
    uint32_t key_length;
    uint8_t key_store[8];
};

bool query_index_term(aegir::bfs::Query const &query, IndexTerm *out) noexcept
{
    aegir::bfs::QueryEquation const *eq = query.single_equation();
    if (eq == nullptr || eq->op != aegir::bfs::kQueryEqual) {
        return false;
    }
    /* The four standard indices share their attribute's name. */
    bool const string_key =
        bytes_match(eq->attribute, eq->attribute_length, "name", 4) ||
        bytes_match(eq->attribute, eq->attribute_length, "BEOS:APP_SIG", 12);
    bool const integer_key =
        bytes_match(eq->attribute, eq->attribute_length, "size", 4) ||
        bytes_match(eq->attribute, eq->attribute_length, "last_modified", 13);
    if (string_key) {
        if (eq->literal != aegir::bfs::QueryLiteral::String ||
            eq->value_length == 0) {
            return false;
        }
        out->key = reinterpret_cast<uint8_t const *>(eq->value);
        out->key_length = eq->value_length;
    } else if (integer_key) {
        if (eq->literal != aegir::bfs::QueryLiteral::Integer &&
            eq->literal != aegir::bfs::QueryLiteral::Unsigned) {
            return false;
        }
        int64_t const number = eq->literal == aegir::bfs::QueryLiteral::Integer
                                   ? eq->integer
                                   : static_cast<int64_t>(eq->unsigned_integer);
        aegir::bfs::put_le64(out->key_store, static_cast<uint64_t>(number));
        out->key = out->key_store;
        out->key_length = 8;
    } else {
        return false;
    }
    if (out->key_length == 0 || out->key_length > aegir::bfs::kMaxName) {
        return false;
    }
    out->name = eq->attribute;
    out->name_length = eq->attribute_length;
    return true;
}

/* Answer one query entry. The end status instead when the entry does not fit
 * a message; either way a reply has been sent. */
void query_emit(aegir::ipc::Owner &port, aegir::bfs::Inode const &inode) noexcept
{
    uint64_t answer[aegir::ipc::kMaxWords];
    uint32_t const name_words = aegir::nmspace::pack_string(
        answer + 1, inode.name, inode.name_length,
        aegir::ipc::kMaxWords * 8 - aegir::metadata::kQueryTailWords * 8);
    if (name_words == 0 ||
        1 + name_words + aegir::metadata::kQueryTailWords > aegir::ipc::kMaxWords) {
        uint64_t const status = aegir::metadata::kNotFound;
        port.reply_words(&status, 1);
        return;
    }
    answer[0] = aegir::metadata::kOk;
    answer[1 + name_words] =
        is_directory(inode) ? 0 : static_cast<uint64_t>(inode.size);
    answer[1 + name_words + 1] =
        is_directory(inode) ? aegir::volume::kKindDir : aegir::volume::kKindFile;
    port.reply_words(answer, 1 + name_words + aegir::metadata::kQueryTailWords);
}

void answer_query_open(aegir::ipc::Owner &port, uint64_t const *words,
                       uint32_t count, uint64_t badge) noexcept
{
    uint64_t answer[aegir::metadata::kQueryOpenTailWords] = {
        aegir::metadata::kNotFound, 0,
    };
    char const *text = nullptr;
    uint32_t text_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::metadata::kQueryTextMax,
                                       &text, &text_length)) {
        port.reply_words(answer, 1);
        return;
    }
    uint32_t const text_words = 1 + (text_length + 7) / 8;
    if (count < text_words + 1 || text_length == 0) {
        port.reply_words(answer, 1);
        return;
    }
    uint64_t const flags = words[text_words];
    if ((flags & aegir::metadata::kQueryFlagLive) != 0) {
        /* Live queries arrive with the notification endpoint; until then a
         * refusal, not a quiet ordinary query. */
        answer[0] = aegir::metadata::kUnsupported;
        port.reply_words(answer, 1);
        return;
    }
    if (!g_query.parse(text, text_length)) {
        answer[0] = aegir::metadata::kInvalidName;
        port.reply_words(answer, 1);
        return;
    }
    Handle *row = handle_alloc(badge);
    if (row == nullptr) {
        port.reply_words(answer, 1);
        return;
    }
    row->kind = 1;
    row->cursor = 1; /* the scan starts at the first block */
    row->index_block = 0;
    row->index_position = 0;
    /* A lone equality on an indexed attribute answers from that index; a
     * missing index falls back to the scan. */
    IndexTerm term;
    uint64_t index_block = 0;
    if (query_index_term(g_query, &term) &&
        g_volume.index_inode(term.name, term.name_length, &index_block)) {
        row->index_block = index_block;
    }
    row->query_length = text_length;
    for (uint32_t i = 0; i < text_length; ++i) {
        row->query[i] = text[i];
    }
    answer[0] = aegir::metadata::kOk;
    answer[1] = row->serial;
    port.reply_words(answer, aegir::metadata::kQueryOpenTailWords);
}

void answer_query_next(aegir::ipc::Owner &port, uint64_t const *words,
                       uint32_t count, uint64_t badge) noexcept
{
    uint64_t status = aegir::metadata::kNotFound;
    if (count < 1) {
        port.reply_words(&status, 1);
        return;
    }
    Handle *handle = handle_lookup(words[0], badge);
    if (handle == nullptr || handle->kind != 1) {
        port.reply_words(&status, 1);
        return;
    }
    if (!g_query.parse(handle->query, handle->query_length)) {
        port.reply_words(&status, 1);
        return;
    }
    /* An indexed term walks the index; each value is still read back and
     * re-checked, so a stale or shared key entry cannot lie. */
    if (handle->index_block != 0) {
        IndexTerm term;
        if (query_index_term(g_query, &term)) {
            uint64_t value = 0;
            while (g_volume.index_walk(handle->index_block, term.key,
                                       term.key_length, &handle->index_position,
                                       &value)) {
                aegir::bfs::Inode inode;
                if (!g_volume.read_inode(value, &inode) || !is_queryable(inode) ||
                    !g_query.matches(g_volume, inode)) {
                    continue;
                }
                query_emit(port, inode);
                return;
            }
        }
        port.reply_words(&status, 1);
        return;
    }
    uint64_t block = handle->cursor;
    aegir::bfs::Inode inode;
    while (g_volume.next_inode(&block, &inode)) {
        if (is_queryable(inode) && g_query.matches(g_volume, inode)) {
            handle->cursor = block;
            query_emit(port, inode);
            return;
        }
    }
    handle->cursor = block;
    port.reply_words(&status, 1);
}

void answer_query_close(aegir::ipc::Owner &port, uint64_t const *words,
                        uint32_t count, uint64_t badge) noexcept
{
    uint64_t closed = 0;
    if (count >= 1) {
        Handle *handle = handle_lookup(words[0], badge);
        if (handle != nullptr && handle->kind == 1) {
            handle->serial = 0;
            closed = 1;
        }
    }
    port.reply_words(&closed, 1);
}

void answer_refuse(aegir::ipc::Owner &port) noexcept
{
    /* The write side is the growth phase: a refusal, not a lie. */
    port.reply_words(nullptr, 0);
}

/* The metadata protocol wants a path and an attribute name, two strings in
 * sequence (aegir/metadata.h). `*name` is left null when the message does not
 * carry one. */
bool unpack_path_name(uint64_t const *words, uint32_t count, char const **path,
                      uint32_t *path_length, char const **name,
                      uint32_t *name_length) noexcept
{
    if (!aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax,
                                       path, path_length)) {
        return false;
    }
    uint32_t const path_words = 1 + (*path_length + 7) / 8;
    if (count <= path_words) {
        return false;
    }
    return aegir::nmspace::unpack_string(words + path_words, count - path_words,
                                         aegir::metadata::kAttrNameMax, name,
                                         name_length);
}

/* A path and a name that could name an attribute: false with the status the
 * client should read when either is wrong. */
bool attr_target(char const *path, uint32_t path_length, char const *name,
                 uint32_t name_length, aegir::bfs::Inode *inode,
                 uint64_t *status) noexcept
{
    if (name == nullptr || name_length == 0 ||
        name_length > aegir::metadata::kAttrNameMax) {
        *status = aegir::metadata::kInvalidName;
        return false;
    }
    if (!walk(path, path_length, inode)) {
        *status = aegir::metadata::kNotFound;
        return false;
    }
    return true;
}

void answer_attr_stat(aegir::ipc::Owner &port, uint64_t const *words,
                      uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    char const *name = nullptr;
    uint32_t name_length = 0;
    aegir::bfs::Inode inode;
    uint64_t status = aegir::metadata::kNotFound;
    if (unpack_path_name(words, count, &path, &path_length, &name, &name_length) &&
        attr_target(path, path_length, name, name_length, &inode, &status)) {
        uint32_t type = 0;
        uint64_t size = 0;
        if (g_volume.attr_stat(inode, name, name_length, &type, &size)) {
            uint64_t const answer[aegir::metadata::kAttrStatTailWords + 1] = {
                aegir::metadata::kOk, type, size,
            };
            port.reply_words(answer, aegir::metadata::kAttrStatTailWords + 1);
            return;
        }
    }
    port.reply_words(&status, 1);
}

void answer_attr_read(aegir::ipc::Owner &port, uint64_t const *words,
                      uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    char const *name = nullptr;
    uint32_t name_length = 0;
    if (!unpack_path_name(words, count, &path, &path_length, &name, &name_length)) {
        uint64_t const status = aegir::metadata::kNotFound;
        port.reply_words(&status, 1);
        return;
    }
    uint32_t const name_words = 1 + (name_length + 7) / 8;
    uint32_t const after = 1 + (path_length + 7) / 8 + name_words;
    uint64_t const status = aegir::metadata::kNotFound;
    if (count < after + 2) {
        port.reply_words(&status, 1);
        return;
    }
    uint64_t const offset = words[after];
    uint64_t wanted = words[after + 1];
    aegir::bfs::Inode inode;
    uint64_t target_status = aegir::metadata::kNotFound;
    if (!attr_target(path, path_length, name, name_length, &inode,
                     &target_status)) {
        port.reply_words(&target_status, 1);
        return;
    }
    if (wanted > aegir::metadata::kAttrDataMax) {
        wanted = aegir::metadata::kAttrDataMax;
    }
    uint64_t answer[aegir::metadata::kAttrReadHeaderWords +
                    aegir::metadata::kAttrDataMax / 8] = {};
    uint32_t length = static_cast<uint32_t>(wanted);
    auto *bytes = reinterpret_cast<uint8_t *>(
        answer + aegir::metadata::kAttrReadHeaderWords);
    if (!g_volume.attr_read(inode, name, name_length, offset, bytes, &length)) {
        port.reply_words(&status, 1);
        return;
    }
    answer[0] = aegir::metadata::kOk;
    answer[1] = length;
    port.reply_words(answer,
                     aegir::metadata::kAttrReadHeaderWords +
                         static_cast<uint32_t>((length + 7) / 8));
}

void answer_attr_write(aegir::ipc::Owner &port, uint64_t const *words,
                       uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    char const *name = nullptr;
    uint32_t name_length = 0;
    if (!unpack_path_name(words, count, &path, &path_length, &name, &name_length)) {
        uint64_t const status = aegir::metadata::kNotFound;
        port.reply_words(&status, 1);
        return;
    }
    uint32_t const after =
        1 + (path_length + 7) / 8 + 1 + (name_length + 7) / 8;
    if (count < after + 3) {
        uint64_t const status = aegir::metadata::kNotFound;
        port.reply_words(&status, 1);
        return;
    }
    uint64_t const type = words[after];
    uint64_t const offset = words[after + 1];
    uint64_t const length = words[after + 2];
    uint64_t answer[aegir::metadata::kAttrWriteTailWords] = {
        aegir::metadata::kNoSpace, 0,
    };
    if (!g_writable) {
        answer[0] = aegir::metadata::kReadOnly;
        port.reply_words(answer, 1);
        return;
    }
    if (length > aegir::metadata::kAttrDataMax ||
        count < after + 3 + static_cast<uint32_t>((length + 7) / 8)) {
        answer[0] = aegir::metadata::kNoSpace;
        port.reply_words(answer, 1);
        return;
    }
    aegir::bfs::Inode inode;
    uint64_t target_status = aegir::metadata::kNotFound;
    if (!attr_target(path, path_length, name, name_length, &inode,
                     &target_status)) {
        port.reply_words(&target_status, 1);
        return;
    }
    auto const *bytes = reinterpret_cast<uint8_t const *>(words + after + 3);
    uint32_t written = 0;
    uint64_t const block = g_volume.to_block(inode.run);
    if (g_writer.attr_write(block, name, name_length, static_cast<uint32_t>(type),
                            offset, bytes, static_cast<uint32_t>(length),
                            &written, inode_time())) {
        answer[0] = aegir::metadata::kOk;
        answer[1] = written;
        port.reply_words(answer, aegir::metadata::kAttrWriteTailWords);
        return;
    }
    port.reply_words(answer, 1);
}

void answer_attr_remove(aegir::ipc::Owner &port, uint64_t const *words,
                        uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    char const *name = nullptr;
    uint32_t name_length = 0;
    uint64_t answer = aegir::metadata::kNotFound;
    if (!unpack_path_name(words, count, &path, &path_length, &name, &name_length)) {
        port.reply_words(&answer, 1);
        return;
    }
    if (!g_writable) {
        answer = aegir::metadata::kReadOnly;
        port.reply_words(&answer, 1);
        return;
    }
    aegir::bfs::Inode inode;
    uint64_t target_status = aegir::metadata::kNotFound;
    if (!attr_target(path, path_length, name, name_length, &inode,
                     &target_status)) {
        port.reply_words(&target_status, 1);
        return;
    }
    uint64_t const block = g_volume.to_block(inode.run);
    answer = g_writer.attr_remove(block, name, name_length)
                 ? aegir::metadata::kOk
                 : aegir::metadata::kNotFound;
    port.reply_words(&answer, 1);
}

void answer_attr_list(aegir::ipc::Owner &port, uint64_t const *words,
                      uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax,
                                       &path, &path_length)) {
        uint64_t const status = aegir::metadata::kNotFound;
        port.reply_words(&status, 1);
        return;
    }
    uint32_t const path_words = 1 + (path_length + 7) / 8;
    uint64_t const status = aegir::metadata::kNotFound;
    if (count < path_words + 1) {
        port.reply_words(&status, 1);
        return;
    }
    aegir::bfs::Inode inode;
    if (!walk(path, path_length, &inode)) {
        port.reply_words(&status, 1);
        return;
    }
    char name[aegir::metadata::kAttrNameMax];
    uint32_t name_length = 0;
    uint32_t type = 0;
    uint64_t size = 0;
    if (!g_volume.attr_entry(inode, static_cast<uint32_t>(words[path_words]), name,
                             &name_length, &type, &size)) {
        port.reply_words(&status, 1);
        return;
    }
    uint64_t answer[aegir::ipc::kMaxWords] = {};
    uint32_t const name_words = aegir::nmspace::pack_string(
        answer + 1, name, name_length,
        aegir::ipc::kMaxWords * 8 - aegir::metadata::kAttrListTailWords * 8);
    if (name_words == 0 ||
        1 + name_words + aegir::metadata::kAttrListTailWords >
            aegir::ipc::kMaxWords) {
        port.reply_words(&status, 1);
        return;
    }
    answer[0] = aegir::metadata::kOk;
    answer[1 + name_words] = type;
    answer[1 + name_words + 1] = size;
    port.reply_words(answer, 1 + name_words + aegir::metadata::kAttrListTailWords);
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

    if (!g_volume.open(read_sector, nullptr, g_writable ? write_sector : nullptr)) {
        aegir::debug_write("      FAIL fs.bfs: not a Be File System this reader speaks\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    if (g_writable && !g_writer.open(&g_volume)) {
        aegir::debug_write("      FAIL fs.bfs: the writer would not open\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The handle table's page, when the spawner gave one: zeroed before it is
     * trusted, because a retyped frame holds whatever the last owner left and
     * a nonzero serial would be a handle nobody opened. */
    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    if (aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address) &&
        memory_bits != 0 && memory_address != 0) {
        g_memory = reinterpret_cast<uint8_t *>(memory_address);
        g_memory_bytes = 1u << memory_bits;
        for (uint32_t i = 0; i < g_memory_bytes; ++i) {
            g_memory[i] = 0;
        }
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

    /* The clock, when the partition manager passed one: made entries and
     * written files are stamped with the time it answers. */
    g_clock = aegir::ipc::Consumer::find(aegir::clock::kPortName,
                                         aegir::clock::kPortNameLength);
    g_have_clock = g_clock.valid();

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
        case aegir::volume::kMethodOpen:
            answer_open(vol, words, count, badge);
            break;
        case aegir::volume::kMethodWrite:
            answer_write(vol, words, count, badge);
            break;
        case aegir::volume::kMethodClose:
            answer_close(vol, words, count, badge);
            break;
        case aegir::volume::kMethodMkdir:
            answer_mkdir(vol, words, count);
            break;
        case aegir::volume::kMethodRemove:
            answer_remove(vol, words, count);
            break;
        case aegir::volume::kMethodRename:
            answer_rename(vol, words, count);
            break;
        case aegir::volume::kMethodTruncate:
            answer_truncate(vol, words, count);
            break;
        case aegir::volume::kMethodReap:
            answer_reap(vol, words, count);
            break;
        case aegir::metadata::kMethodAttrStat:
            answer_attr_stat(vol, words, count);
            break;
        case aegir::metadata::kMethodAttrRead:
            answer_attr_read(vol, words, count);
            break;
        case aegir::metadata::kMethodAttrWrite:
            answer_attr_write(vol, words, count);
            break;
        case aegir::metadata::kMethodAttrRemove:
            answer_attr_remove(vol, words, count);
            break;
        case aegir::metadata::kMethodAttrList:
            answer_attr_list(vol, words, count);
            break;
        case aegir::metadata::kMethodQueryOpen:
            answer_query_open(vol, words, count, badge);
            break;
        case aegir::metadata::kMethodQueryNext:
            answer_query_next(vol, words, count, badge);
            break;
        case aegir::metadata::kMethodQueryClose:
            answer_query_close(vol, words, count, badge);
            break;
        case aegir::metadata::kMethodQueryOpenLive: {
            uint64_t const status = aegir::metadata::kUnsupported;
            vol.reply_words(&status, 1);
            break;
        }
        default:
            /* The metadata methods and queries: the next phases, refused
             * rather than answered wrongly. */
            answer_refuse(vol);
            break;
        }
    }
}
