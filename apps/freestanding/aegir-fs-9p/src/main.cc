/*
 * aegir-fs-9p: the 9P filesystem service.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The host shares a directory and the machine mounts it: this service opens the
 * bound p9.* transport through the device manager's registry, maps the window
 * it serves through, runs a 9P session over it (aegir/p9client.h), and answers
 * the volume protocol (aegir/volume.h) by turning each call into a walk and a
 * message. It registers the export's own tag as a volume, so `list`, `type`
 * and the rest reach the host directory by the ordinary file protocol -- which
 * is the whole reason the transport exists (specs/9p.md).
 *
 * A read path is stateless: walk from the attached root, open, read, clunk. A
 * write path is a handle -- the serial, its badge, the server fid and the
 * cursor -- held in a table retyped from the service's untyped, so a write
 * continues where the last one left off and a close or a reap clunks the fid.
 * The same core functions serve the volume port and the self-test, so what the
 * boot proves is what a command reaches.
 */

#include <aegir/9p.h>
#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/nmspace.h>
#include <aegir/p9client.h>
#include <aegir/p9transport.h>
#include <aegir/registry.h>
#include <aegir/volume.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

using aegir::p9::Client;
using aegir::p9::Name;
using aegir::p9::Qid;

void write(char const *text) noexcept
{
    aegir::debug_write(text);
}

void write(char const *text, uint32_t length) noexcept
{
    aegir::debug_write(text, length);
}

void write_unsigned(uint64_t value) noexcept
{
    aegir::debug_write_unsigned(value);
}

/* The maps grant: our own objects, the window we map the transport's frames and
 * the handle table through, and what an allocation is charged to. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);
aegir::mem::Account g_account{"fs-9p", 0, 0, 0};

/* The transport driver's port and the window it serves through, mapped into our
 * own address space at consecutive pages. */
aegir::ipc::Consumer g_transport;
uint8_t *g_window = nullptr;
uint32_t g_window_bytes = 0;

/* The export's name, read from the transport's `info`. */
char g_tag[aegir::p9transport::kTagMax];
uint32_t g_tag_length = 0;

/* Fids are the client's own; 0 is the attached root and is never clunked. */
uint32_t g_fid_next = 1;

/* The path a list entry's own getattr is walked at: the directory's path, a
 * slash, the entry's name. Static and generously sized because the service is
 * single-threaded and a deep path plus a long name can approach kPathMax. */
char g_child[aegir::nmspace::kPathMax + 256];

/* The write side's handle table, retyped from the service's untyped and mapped
 * into our window: each row is one open file -- the serial the client names,
 * whose badge it is, the server fid, and the write cursor. The table's bound is
 * the grant, and reaching it is a refusal, never a quiet overwrite
 * (specs/vfs.md). */
struct Handle {
    uint64_t serial;
    uint64_t badge;
    uint32_t fid;
    uint64_t cursor;
};

uint8_t *g_handles = nullptr;
uint32_t g_handles_bytes = 0;
uint64_t g_handle_serial = 0;

uint32_t handle_capacity() noexcept
{
    return g_handles_bytes / static_cast<uint32_t>(sizeof(Handle));
}

Handle *handle_lookup(uint64_t serial, uint64_t badge) noexcept
{
    if (serial == 0 || g_handles == nullptr) {
        return nullptr;
    }
    auto *rows = reinterpret_cast<Handle *>(g_handles);
    for (uint32_t i = 0; i < handle_capacity(); ++i) {
        if (rows[i].serial == serial) {
            return rows[i].badge == badge ? rows + i : nullptr;
        }
    }
    return nullptr;
}

Handle *handle_alloc(uint64_t badge) noexcept
{
    if (g_handles == nullptr) {
        return nullptr;
    }
    auto *rows = reinterpret_cast<Handle *>(g_handles);
    for (uint32_t i = 0; i < handle_capacity(); ++i) {
        if (rows[i].serial == 0) {
            rows[i].serial = ++g_handle_serial;
            rows[i].badge = badge;
            rows[i].fid = 0;
            rows[i].cursor = 0;
            return rows + i;
        }
    }
    return nullptr;
}

/* The transport the client speaks through: the virtio driver's window is both
 * buffers, the request at the start and the reply in the second half
 * (aegir/p9transport.h). */
class VirtioTransport : public aegir::p9transport::Transport {
public:
    uint8_t *request_buffer() noexcept override { return g_window; }

    uint32_t request_capacity() noexcept override
    {
        return aegir::p9transport::request_bytes(g_window_bytes);
    }

    bool round_trip(uint32_t length) noexcept override
    {
        uint64_t const words[1] = {length};
        uint64_t answer[1] = {0};
        aegir::ipc::WordsReply const reply = g_transport.call_words(
            aegir::p9transport::kMethodRoundTrip, words, 1, answer, 1);
        if (reply.error != 0 || reply.count < 1 || answer[0] < aegir::p9::kHeaderBytes) {
            return false;
        }
        reply_length_ = static_cast<uint32_t>(answer[0]);
        return true;
    }

    uint8_t const *reply() noexcept override
    {
        return g_window + aegir::p9transport::reply_offset(g_window_bytes);
    }

    uint32_t reply_length() noexcept override { return reply_length_; }

private:
    uint32_t reply_length_ = 0;
};

VirtioTransport g_transport_adapter;
Client g_client(&g_transport_adapter);

uint32_t alloc_fid() noexcept
{
    uint32_t const fid = g_fid_next;
    ++g_fid_next;
    if (g_fid_next == 0xfffffffeu) {
        g_fid_next = 1;
    }
    return fid;
}

/** A fresh fid that names the same file as `fid` (a zero-name Twalk). */
uint32_t clone_fid(uint32_t fid) noexcept
{
    uint32_t const next = alloc_fid();
    int32_t const walked = g_client.walk(fid, next, nullptr, 0, nullptr, 0);
    return walked == 0 ? next : aegir::p9::kNoFid;
}

/** One chunk of components walked in a single Twalk: a path is walked a handful
 *  at a time so neither a Name array nor the message has to hold the whole
 *  path at once. */
constexpr uint32_t kWalkChunk = 16;

/** Walk `path` (slash-separated; empty is the root) from `start` into a fresh
 *  fid. Answers that fid, or kNoFid when any component is missing; `start` is
 *  left alone, and for an empty path the fresh fid is a clone of it. */
uint32_t walk_path(uint32_t start, char const *path, uint32_t length, Qid *last) noexcept
{
    uint32_t current = start;
    bool moved = false;
    bool first = true;
    uint32_t at = 0;
    while (first || at < length) {
        first = false;
        Name names[kWalkChunk];
        uint32_t count = 0;
        while (count < kWalkChunk && at < length) {
            uint32_t end = at;
            while (end < length && path[end] != '/') {
                ++end;
            }
            if (end > at) {
                names[count].data = path + at;
                names[count].length = end - at;
                ++count;
            }
            at = end + 1;
        }
        Qid qids[kWalkChunk];
        uint32_t const next = alloc_fid();
        int32_t const walked = g_client.walk(current, next, names, count, qids, kWalkChunk);
        if (moved) {
            (void)g_client.clunk(current);
        }
        if (walked < 0 || static_cast<uint32_t>(walked) != count) {
            (void)g_client.clunk(next);
            return aegir::p9::kNoFid;
        }
        if (count > 0 && last != nullptr) {
            *last = qids[count - 1];
        }
        current = next;
        moved = true;
    }
    return current;
}

/** Split `path` into its parent directory and its final name. False when the
 *  path is empty or names the root (a trailing slash): neither has a name to
 *  make, remove or rename. */
bool split_path(char const *path, uint32_t length, uint32_t *parent_length,
                char const **name, uint32_t *name_length) noexcept
{
    if (length == 0) {
        return false;
    }
    uint32_t slash = length; /* no separator: the parent is the root */
    for (uint32_t i = length; i > 0; --i) {
        if (path[i - 1] == '/') {
            slash = i - 1;
            break;
        }
    }
    if (slash == length) {
        *parent_length = 0;
        *name = path;
        *name_length = length;
        return true;
    }
    *parent_length = slash;
    *name = path + slash + 1;
    *name_length = length - slash - 1;
    return *name_length != 0;
}

/* ------------------------------------------------------------------ *
 * The core operations: what the volume port and the self-test both call.
 * ------------------------------------------------------------------ */

/* Open `path` for writing, making it when `kOpenCreate` and cutting it to
 * nothing when `kOpenTruncate`. Answers the handle, or zero. */
uint64_t do_open(char const *path, uint32_t length, uint64_t flags, uint64_t badge) noexcept
{
    uint32_t parent_length = 0;
    char const *name = nullptr;
    uint32_t name_length = 0;
    if (!split_path(path, length, &parent_length, &name, &name_length)) {
        return 0;
    }
    uint32_t const dfid = walk_path(0, path, parent_length, nullptr);
    if (dfid == aegir::p9::kNoFid) {
        return 0;
    }
    uint32_t ffid = walk_path(dfid, name, name_length, nullptr);
    bool opened = false;
    if (ffid != aegir::p9::kNoFid) {
        opened = g_client.open(ffid, aegir::p9::kOWrite);
        if (opened && (flags & aegir::volume::kOpenTruncate) != 0) {
            opened = g_client.setattr_size(ffid, 0);
        }
    } else if ((flags & aegir::volume::kOpenCreate) != 0) {
        ffid = clone_fid(dfid);
        opened = ffid != aegir::p9::kNoFid &&
                 g_client.create(ffid, name, name_length, aegir::p9::kOWrite, 0644, 0);
    }
    if (!opened) {
        if (ffid != aegir::p9::kNoFid) {
            (void)g_client.clunk(ffid);
        }
        (void)g_client.clunk(dfid);
        return 0;
    }
    (void)g_client.clunk(dfid);
    Handle *handle = handle_alloc(badge);
    if (handle == nullptr) {
        (void)g_client.clunk(ffid);
        return 0;
    }
    handle->fid = ffid;
    return handle->serial;
}

uint32_t do_write(uint64_t serial, uint64_t badge, uint8_t const *bytes,
                  uint32_t count) noexcept
{
    Handle *handle = handle_lookup(serial, badge);
    if (handle == nullptr) {
        return 0;
    }
    int32_t const written = g_client.write(handle->fid, handle->cursor, bytes, count);
    if (written <= 0) {
        return 0;
    }
    handle->cursor += static_cast<uint64_t>(written);
    return static_cast<uint32_t>(written);
}

bool do_close(uint64_t serial, uint64_t badge) noexcept
{
    Handle *handle = handle_lookup(serial, badge);
    if (handle == nullptr) {
        return false;
    }
    (void)g_client.clunk(handle->fid);
    handle->serial = 0;
    return true;
}

bool do_mkdir(char const *path, uint32_t length) noexcept
{
    uint32_t parent_length = 0;
    char const *name = nullptr;
    uint32_t name_length = 0;
    if (!split_path(path, length, &parent_length, &name, &name_length)) {
        return false;
    }
    uint32_t const dfid = walk_path(0, path, parent_length, nullptr);
    if (dfid == aegir::p9::kNoFid) {
        return false;
    }
    bool const ok = g_client.mkdir(dfid, name, name_length, 0755, 0);
    (void)g_client.clunk(dfid);
    return ok;
}

bool do_remove(char const *path, uint32_t length) noexcept
{
    uint32_t parent_length = 0;
    char const *name = nullptr;
    uint32_t name_length = 0;
    if (!split_path(path, length, &parent_length, &name, &name_length)) {
        return false;
    }
    uint32_t const dfid = walk_path(0, path, parent_length, nullptr);
    if (dfid == aegir::p9::kNoFid) {
        return false;
    }
    uint32_t const target = walk_path(dfid, name, name_length, nullptr);
    if (target == aegir::p9::kNoFid) {
        (void)g_client.clunk(dfid);
        return false;
    }
    Client::Attr attr{};
    bool const is_dir = g_client.getattr(target, &attr) && attr.is_dir;
    (void)g_client.clunk(target);
    /* AT_REMOVEDIR for a directory; the flag is the only difference. */
    bool const ok = g_client.unlinkat(dfid, name, name_length, is_dir ? 0x200u : 0u);
    (void)g_client.clunk(dfid);
    return ok;
}

bool do_rename(char const *src, uint32_t src_length, char const *dst,
               uint32_t dst_length) noexcept
{
    uint32_t src_parent = 0;
    char const *src_name = nullptr;
    uint32_t src_name_length = 0;
    uint32_t dst_parent = 0;
    char const *dst_name = nullptr;
    uint32_t dst_name_length = 0;
    if (!split_path(src, src_length, &src_parent, &src_name, &src_name_length) ||
        !split_path(dst, dst_length, &dst_parent, &dst_name, &dst_name_length)) {
        return false;
    }
    uint32_t const sfid = walk_path(0, src, src_parent, nullptr);
    if (sfid == aegir::p9::kNoFid) {
        return false;
    }
    uint32_t const dfid = walk_path(0, dst, dst_parent, nullptr);
    if (dfid == aegir::p9::kNoFid) {
        (void)g_client.clunk(sfid);
        return false;
    }
    bool const ok = g_client.renameat(sfid, src_name, src_name_length, dfid, dst_name,
                                      dst_name_length);
    (void)g_client.clunk(sfid);
    (void)g_client.clunk(dfid);
    return ok;
}

bool do_truncate(char const *path, uint32_t length, uint64_t size) noexcept
{
    uint32_t const fid = walk_path(0, path, length, nullptr);
    if (fid == aegir::p9::kNoFid) {
        return false;
    }
    bool const ok = g_client.setattr_size(fid, size);
    (void)g_client.clunk(fid);
    return ok;
}

/* ------------------------------------------------------------------ *
 * The volume protocol (aegir/volume.h).
 * ------------------------------------------------------------------ */

/* read: a path, an offset, how many bytes. The bytes are copied into the answer
 * *before* the clunk (which is another message and would overwrite the reply). */
void answer_read(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                 uint64_t badge) noexcept
{
    static_cast<void>(badge);
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 || !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax,
                                                     &path, &path_length)) {
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
    if (wanted > aegir::volume::kReadMax) {
        wanted = aegir::volume::kReadMax;
    }

    uint64_t answer[aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8];
    bool ok = false;
    uint32_t got = 0;
    bool eof = false;
    uint32_t const fid = walk_path(0, path, path_length, nullptr);
    if (fid != aegir::p9::kNoFid) {
        if (g_client.open(fid, aegir::p9::kORead)) {
            uint8_t const *data = nullptr;
            int32_t const n = g_client.read(fid, offset, static_cast<uint32_t>(wanted), &data);
            if (n >= 0) {
                got = static_cast<uint32_t>(n);
                eof = got < wanted;
                char *bytes = reinterpret_cast<char *>(answer + aegir::volume::kReadHeaderWords);
                for (uint32_t i = 0; i < got; ++i) {
                    bytes[i] = static_cast<char>(data[i]);
                }
                ok = true;
            }
        }
        (void)g_client.clunk(fid);
    }
    if (!ok) {
        port.reply_words(nullptr, 0);
        return;
    }
    answer[0] = got;
    answer[1] = eof ? 1 : 0;
    port.reply_words(answer, aegir::volume::kReadHeaderWords +
                                 static_cast<uint32_t>((got + 7) / 8));
}

/* list: a path and an index. A directory is opened and read chunk by chunk
 * until the index-th entry is found; the entry's own size and time are then a
 * walk and a getattr, because a 9P dirent carries neither. */
void answer_list(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                 uint64_t badge) noexcept
{
    static_cast<void>(badge);
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 || !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax,
                                                     &path, &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint32_t const path_words = 1 + (path_length + 7) / 8;
    if (count < path_words + 1) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const index = words[path_words];

    char name[256];
    uint32_t name_length = 0;
    uint64_t kind = 0;
    bool found = false;
    uint32_t const dfid = walk_path(0, path, path_length, nullptr);
    if (dfid != aegir::p9::kNoFid && g_client.open(dfid, aegir::p9::kORead)) {
        uint64_t offset = 0;
        uint32_t seen = 0;
        for (;;) {
            uint8_t const *blob = nullptr;
            uint32_t blob_length = 0;
            if (!g_client.readdir(dfid, offset, 4096, &blob, &blob_length) ||
                blob_length == 0) {
                break;
            }
            aegir::p9::Reader reader(blob, blob_length);
            uint64_t last_offset = offset;
            while (reader.remaining() >= aegir::p9::kQidBytes + 8 + 1) {
                reader.get_bytes(aegir::p9::kQidBytes);
                uint64_t const entry_offset = reader.get_u64();
                uint8_t const entry_type = reader.get_u8();
                uint8_t const *entry_name = nullptr;
                uint32_t entry_name_length = 0;
                reader.get_string(&entry_name, &entry_name_length);
                if (!reader.ok()) {
                    break;
                }
                last_offset = entry_offset;
                if (seen == index) {
                    uint32_t const n = entry_name_length < sizeof(name)
                                           ? entry_name_length
                                           : static_cast<uint32_t>(sizeof(name));
                    for (uint32_t i = 0; i < n; ++i) {
                        name[i] = static_cast<char>(entry_name[i]);
                    }
                    name_length = n;
                    kind = (entry_type & aegir::p9::kQtdir) != 0 ? aegir::volume::kKindDir
                                                                 : aegir::volume::kKindFile;
                    found = true;
                    break;
                }
                ++seen;
            }
            if (found || last_offset == offset) {
                break;
            }
            offset = last_offset;
        }
        (void)g_client.clunk(dfid);
    }
    if (!found) {
        port.reply_words(nullptr, 0);
        return;
    }

    /* The entry's size and time: build its path and getattr it. The name was
     * copied out above because the readdir blob died with the walk. */
    uint32_t at = 0;
    for (uint32_t i = 0; i < path_length && at < sizeof(g_child) - 1; ++i) {
        g_child[at++] = path[i];
    }
    if (path_length != 0 && at < sizeof(g_child) - 1) {
        g_child[at++] = '/';
    }
    for (uint32_t i = 0; i < name_length && at < sizeof(g_child) - 1; ++i) {
        g_child[at++] = name[i];
    }
    uint64_t size = 0;
    uint64_t mtime = 0;
    uint32_t const cfid = walk_path(0, g_child, at, nullptr);
    if (cfid != aegir::p9::kNoFid) {
        Client::Attr attr{};
        if (g_client.getattr(cfid, &attr)) {
            size = attr.size;
            mtime = attr.mtime;
            kind = attr.is_dir ? aegir::volume::kKindDir : aegir::volume::kKindFile;
        }
        (void)g_client.clunk(cfid);
    }

    uint64_t answer[aegir::ipc::kMaxWords];
    uint32_t const name_words = aegir::nmspace::pack_string(
        answer, name, name_length, aegir::nmspace::kNameMax);
    if (name_words == 0 ||
        name_words + aegir::volume::kListTailWords > aegir::ipc::kMaxWords) {
        port.reply_words(nullptr, 0);
        return;
    }
    answer[name_words] = size;
    answer[name_words + 1] = kind;
    answer[name_words + 2] = mtime;
    port.reply_words(answer, name_words + aegir::volume::kListTailWords);
}

/* stat: a path. The walk ends at the thing it names and a getattr answers its
 * kind, size and time. A directory has no size, so its size word is zero. */
void answer_stat(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                 uint64_t badge) noexcept
{
    static_cast<void>(badge);
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 || !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax,
                                                     &path, &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t kind = 0;
    uint64_t size = 0;
    uint64_t mtime = 0;
    bool ok = false;
    uint32_t const fid = walk_path(0, path, path_length, nullptr);
    if (fid != aegir::p9::kNoFid) {
        Client::Attr attr{};
        if (g_client.getattr(fid, &attr)) {
            kind = attr.is_dir ? aegir::volume::kKindDir : aegir::volume::kKindFile;
            size = attr.is_dir ? 0 : attr.size;
            mtime = attr.mtime;
            ok = true;
        }
        (void)g_client.clunk(fid);
    }
    if (!ok) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const answer[aegir::volume::kStatTailWords] = {kind, size, mtime};
    port.reply_words(answer, aegir::volume::kStatTailWords);
}

/* space: the export's capacity, from a statfs on the attached root. */
void answer_space(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                  uint64_t badge) noexcept
{
    static_cast<void>(badge);
    static_cast<void>(words);
    static_cast<void>(count);
    Client::Statfs stats{};
    if (!g_client.statfs(0, &stats)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const answer[aegir::volume::kSpaceTailWords] = {stats.total, stats.free};
    port.reply_words(answer, aegir::volume::kSpaceTailWords);
}

/* open: a path and mode flags. A read open answers no handle (the hosted file
 * layer falls back to path reads); a write open answers the handle do_open
 * makes, or zero on refusal. */
void answer_open(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                 uint64_t badge) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 || !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax,
                                                     &path, &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint32_t const path_words = 1 + (path_length + 7) / 8;
    if (count < path_words + 1) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const flags = words[path_words];
    uint64_t handle = 0;
    if ((flags & aegir::volume::kOpenRead) == 0) {
        handle = do_open(path, path_length, flags, badge);
    }
    port.reply_words(&handle, 1);
}

/* write: a handle and the bytes packed after the count. */
void answer_write(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                  uint64_t badge) noexcept
{
    if (count < 2) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const handle = words[0];
    uint64_t const bytes_count = words[1];
    if (bytes_count > aegir::volume::kWriteMax ||
        count < 2 + (bytes_count + 7) / 8) {
        port.reply_words(nullptr, 0);
        return;
    }
    auto const *bytes = reinterpret_cast<uint8_t const *>(words + 2);
    uint32_t const written =
        do_write(handle, badge, bytes, static_cast<uint32_t>(bytes_count));
    uint64_t const answer[1] = {written};
    port.reply_words(answer, 1);
}

void answer_close(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                  uint64_t badge) noexcept
{
    uint64_t const handle = count >= 1 ? words[0] : 0;
    uint64_t const answer[1] = {do_close(handle, badge) ? 1ULL : 0ULL};
    port.reply_words(answer, 1);
}

/* mkdir: a path. */
void answer_mkdir(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                  uint64_t badge) noexcept
{
    static_cast<void>(badge);
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 || !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax,
                                                     &path, &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const answer[1] = {do_mkdir(path, path_length) ? 1ULL : 0ULL};
    port.reply_words(answer, 1);
}

void answer_remove(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                   uint64_t badge) noexcept
{
    static_cast<void>(badge);
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 || !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax,
                                                     &path, &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const answer[1] = {do_remove(path, path_length) ? 1ULL : 0ULL};
    port.reply_words(answer, 1);
}

void answer_rename(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                   uint64_t badge) noexcept
{
    static_cast<void>(badge);
    char const *src = nullptr;
    uint32_t src_length = 0;
    if (count == 0 || !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax,
                                                     &src, &src_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint32_t const src_words = 1 + (src_length + 7) / 8;
    char const *dst = nullptr;
    uint32_t dst_length = 0;
    if (!aegir::nmspace::unpack_string(words + src_words, count - src_words,
                                       aegir::nmspace::kPathMax, &dst, &dst_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const answer[1] = {do_rename(src, src_length, dst, dst_length) ? 1ULL : 0ULL};
    port.reply_words(answer, 1);
}

void answer_truncate(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                     uint64_t badge) noexcept
{
    static_cast<void>(badge);
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 || !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax,
                                                     &path, &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint32_t const path_words = 1 + (path_length + 7) / 8;
    if (count < path_words + 1) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const size = words[path_words];
    uint64_t const answer[1] = {do_truncate(path, path_length, size) ? 1ULL : 0ULL};
    port.reply_words(answer, 1);
}

/* reap: a badge. Every fid its handles name is clunked, as though closed. */
void answer_reap(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                 uint64_t badge) noexcept
{
    static_cast<void>(badge);
    uint64_t const target = count >= 1 ? words[0] : 0;
    uint64_t reaped = 0;
    if (g_handles != nullptr) {
        auto *rows = reinterpret_cast<Handle *>(g_handles);
        for (uint32_t i = 0; i < handle_capacity(); ++i) {
            if (rows[i].serial != 0 && rows[i].badge == target) {
                (void)g_client.clunk(rows[i].fid);
                rows[i].serial = 0;
                ++reaped;
            }
        }
    }
    uint64_t const answer[1] = {reaped};
    port.reply_words(answer, 1);
}

/* ------------------------------------------------------------------ */

/** Where the process's own free slots begin: everything past the capabilities
 *  the bootstrap block named. */
uint64_t first_free_slot() noexcept
{
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            uint64_t const occupied =
                entry.kind == aegir::bootstrap::EntryKind::Capability ? entry.number
                : entry.kind == aegir::bootstrap::EntryKind::DeviceCapability
                    ? entry.reserved
                    : 0;
            if (occupied != 0 && occupied + 1 > first_free) {
                first_free = occupied + 1;
            }
        }
    }
    return first_free;
}

/* The handle table: a run of pages retyped from the untyped and mapped into our
 * window, half the grant so the page tables the window mapping needs still have
 * room. Must run before anything else maps, so the run is contiguous. */
bool handles_init(uint32_t untyped_bits) noexcept
{
    uint32_t pages = (1u << untyped_bits) / 8192u;
    if (pages == 0) {
        return false;
    }
    uint32_t power = 1;
    while (power * 2 <= pages) {
        power *= 2;
    }
    pages = power;
    if (pages * sizeof(Handle) < 2 * sizeof(Handle)) {
        return false;
    }
    uintptr_t const base = g_scratch.reserve(pages);
    if (base == 0) {
        return false;
    }
    seL4_Error error = seL4_NoError;
    seL4_CPtr const run = g_objects.alloc_pages_run(pages, g_account, &error);
    if (run == 0) {
        return false;
    }
    for (uint32_t i = 0; i < pages; ++i) {
        if (!g_scratch.map_at(base + i * 4096u, run + i)) {
            return false;
        }
    }
    g_handles = reinterpret_cast<uint8_t *>(base);
    g_handles_bytes = pages * 4096u;
    return true;
}

/* Open the bound p9.* transport and map the window it serves through into our
 * own address space. The row may not be there yet -- the device manager binds
 * the driver asynchronously -- so the caller retries. */
bool open_transport() noexcept
{
    aegir::ipc::Consumer const registry =
        aegir::ipc::Consumer::find(aegir::registry::kPortName,
                                   aegir::registry::kPortNameLength);
    if (!registry.valid()) {
        return false;
    }
    static char const kName[] = "p9.virtio0";
    int64_t const row = aegir::registry::find_bound(registry, kName, sizeof(kName) - 1);
    if (row < 0) {
        return false;
    }
    seL4_CPtr const port_slot = g_objects.alloc_slot();
    if (port_slot == 0 ||
        !aegir::registry::open_bound(registry, kName, sizeof(kName) - 1, port_slot)) {
        return false;
    }

    uint64_t page_bits = 0;
    uint64_t pages = 0;
    if (!aegir::registry::window_geometry(registry, static_cast<uint64_t>(row), &page_bits,
                                          &pages) ||
        pages == 0 || page_bits != seL4_PageBits) {
        return false;
    }
    uint8_t *window = nullptr;
    for (uint64_t frame = 0; frame < pages; ++frame) {
        seL4_CPtr const frame_slot = g_objects.alloc_slot();
        if (frame_slot == 0 ||
            !aegir::registry::window_frame(registry, static_cast<uint64_t>(row), frame,
                                           frame_slot)) {
            return false;
        }
        void *const at = g_scratch.map(frame_slot);
        if (at == nullptr) {
            return false;
        }
        if (frame == 0) {
            window = static_cast<uint8_t *>(at);
        }
    }
    if (window == nullptr) {
        return false;
    }
    g_transport = aegir::ipc::Consumer(port_slot);
    g_window = window;
    g_window_bytes = static_cast<uint32_t>(pages * (1ull << page_bits));
    return true;
}

/** A whole-file read through the client, for the self-test: walk, open, read
 *  up to `capacity`, clunk. */
bool read_file(char const *path, uint32_t length, char *out, uint32_t capacity,
               uint32_t *got) noexcept
{
    bool ok = false;
    uint32_t const fid = walk_path(0, path, length, nullptr);
    if (fid != aegir::p9::kNoFid) {
        if (g_client.open(fid, aegir::p9::kORead)) {
            uint8_t const *data = nullptr;
            int32_t const n = g_client.read(fid, 0, capacity, &data);
            if (n >= 0) {
                *got = static_cast<uint32_t>(n);
                for (uint32_t i = 0; i < *got; ++i) {
                    out[i] = static_cast<char>(data[i]);
                }
                ok = true;
            }
        }
        (void)g_client.clunk(fid);
    }
    return ok;
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

    /* The maps grant: our own objects, our VSpace root and the window we map the
     * transport's frames and the handle table through. */
    uint64_t untyped_slot = 0;
    uint32_t untyped_bits = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    if (!aegir::bootstrap::capability("untyped", 7, &untyped_slot) ||
        !aegir::bootstrap::capability_size_bits("untyped", 7, &untyped_bits) ||
        !aegir::bootstrap::capability("vspace", 6, &vspace_slot) ||
        !aegir::bootstrap::window(&window_base, &window_bytes)) {
        write("  9p: no untyped, vspace or window was given to me\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    if (!g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits)) {
        write("  9p: the untyped would not be remembered\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    uint64_t const first_free = first_free_slot();
    g_objects.adopt_slots(static_cast<seL4_CPtr>(first_free),
                          (1u << aegir::bootstrap::kCNodeBits) -
                              static_cast<uint32_t>(first_free),
                          0, aegir::bootstrap::kCNodeBits);
    if (!g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                         static_cast<uintptr_t>(window_base),
                         static_cast<uintptr_t>(window_base + window_bytes), &g_objects)) {
        write("  9p: the window would not be adopted\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    seL4_SetCapReceivePath(aegir::bootstrap::kSlotOwnCNode,
                           static_cast<seL4_CPtr>(first_free),
                           aegir::bootstrap::kCNodeBits);

    /* The handle table first, so its run is contiguous; then the transport,
     * retried until the device manager has bound its driver. */
    if (!handles_init(untyped_bits)) {
        write("  9p: no room for the write side's handle table\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    while (!open_transport()) {
        seL4_Yield();
    }

    /* The export's name, then the session: version and attach. */
    {
        uint64_t words[aegir::p9transport::kTagMax / 8 + 2];
        aegir::ipc::WordsReply const info = g_transport.call_words(
            aegir::p9transport::kMethodInfo, nullptr, 0, words,
            aegir::p9transport::kTagMax / 8 + 2);
        char const *tag = nullptr;
        uint32_t tag_length = 0;
        if (info.error != 0 ||
            !aegir::nmspace::unpack_string(words, info.count, aegir::p9transport::kTagMax,
                                           &tag, &tag_length)) {
            write("  9p: the transport named no export\n");
            seL4_Signal(aegir::bootstrap::kSlotSupervision);
            aegir::halt();
        }
        uint32_t const n = tag_length < aegir::p9transport::kTagMax
                               ? tag_length
                               : aegir::p9transport::kTagMax;
        for (uint32_t i = 0; i < n; ++i) {
            g_tag[i] = tag[i];
        }
        g_tag_length = n;
    }
    uint32_t const msize =
        g_client.version(aegir::p9transport::request_bytes(g_window_bytes));
    if (msize == 0 || !g_client.attach(0, "", 0)) {
        write("  9p: the export would not answer a session\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    write("  9p: ");
    write(g_tag, g_tag_length);
    write(", 9P2000.L, msize ");
    write_unsigned(msize);
    write("\n");

    /* The self-test, before `ready`: read a file the runner put in the host
     * directory, so walk, open, read and clunk are proved against the host's
     * own bytes; then write one and read it back, so the same is proved for the
     * write side. The runner checks the host file after the run. */
    {
        static char const kIn[] = "hello.txt";
        char content[256];
        uint32_t got = 0;
        if (read_file(kIn, sizeof(kIn) - 1, content, sizeof(content), &got)) {
            write("  9p: read ");
            write_unsigned(got);
            write(" bytes: ");
            write(content, got);
            write("\n");
        } else {
            write("  9p: FAIL the export would not read hello.txt\n");
        }

        static char const kOut[] = "written.txt";
        static char const kText[] = "Aegir 9P: the machine wrote this through the volume.\n";
        uint32_t const text_length = sizeof(kText) - 1;
        uint64_t const handle =
            do_open(kOut, sizeof(kOut) - 1,
                    aegir::volume::kOpenCreate | aegir::volume::kOpenTruncate, 0);
        uint32_t written = 0;
        if (handle != 0) {
            written = do_write(handle, 0, reinterpret_cast<uint8_t const *>(kText),
                               text_length);
            (void)do_close(handle, 0);
        }
        uint32_t readback = 0;
        bool const read_ok = read_file(kOut, sizeof(kOut) - 1, content, sizeof(content),
                                       &readback);
        if (written != text_length || !read_ok || readback != text_length) {
            write("  9p: FAIL the export would not take a write\n");
        } else {
            write("  9p: wrote ");
            write_unsigned(written);
            write(" bytes, read back: ");
            write(content, readback);
            write("\n");
        }
    }

    /* The volume's caller half, minted unbadged: the VFS badges each resolver's
     * own copy, which a badged cap would make impossible. */
    uint64_t owner_slot = 0;
    static char const kOwner[] = "vol.hostfs";
    if (!aegir::bootstrap::capability(kOwner, sizeof(kOwner) - 1, &owner_slot) ||
        owner_slot == 0) {
        write("  9p: no volume port was given to me\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    seL4_CPtr const caller_half = g_objects.alloc_slot();
    if (caller_half == 0 ||
        seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, caller_half,
                        aegir::bootstrap::kCNodeBits, aegir::bootstrap::kSlotOwnCNode,
                        static_cast<seL4_CPtr>(owner_slot), aegir::bootstrap::kCNodeBits,
                        seL4_CapRights_new(1, 1, 0, 1), 0) != seL4_NoError) {
        write("  9p: the caller half would not mint\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    aegir::ipc::Owner volume = aegir::ipc::Owner::find(kOwner, sizeof(kOwner) - 1);
    aegir::ipc::Consumer const nmspace =
        aegir::ipc::Consumer::find(aegir::nmspace::kPortName, aegir::nmspace::kPortNameLength);
    if (!volume.valid() || !nmspace.valid()) {
        write("  9p: no volume port or no namespace\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* Register the volume, named after the export. It takes writes now, so no
     * read-only flag (specs/9p.md). */
    uint64_t out[aegir::nmspace::kNameMax / 8 + aegir::nmspace::kTypeMax / 8 + 2];
    uint32_t out_words = aegir::nmspace::pack_string(out, g_tag, g_tag_length,
                                                     aegir::nmspace::kNameMax);
    out[out_words++] = aegir::nmspace::kFlagPublic;
    static char const kType[] = "9P";
    out_words += aegir::nmspace::pack_string(out + out_words, kType, sizeof(kType) - 1,
                                             aegir::nmspace::kTypeMax);
    uint64_t in[aegir::nmspace::kNameMax / 8 + 1];
    aegir::ipc::WordsReply const registered = nmspace.call_transfer(
        aegir::nmspace::kMethodRegister, out, out_words, caller_half, in,
        aegir::nmspace::kNameMax / 8 + 1, nullptr);
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, caller_half,
                      aegir::bootstrap::kCNodeBits);
    char const *assigned = nullptr;
    uint32_t assigned_length = 0;
    if (registered.error != 0 || registered.count == 0 ||
        !aegir::nmspace::unpack_string(in, registered.count, aegir::nmspace::kNameMax,
                                       &assigned, &assigned_length)) {
        write("  9p: the namespace refused the registration\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    write("  9p: ");
    write(assigned, assigned_length);
    write(": registered, serving\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        uint64_t badge = 0;
        uint32_t const method =
            volume.receive_words(words, aegir::ipc::kMaxWords, &count, &badge);
        switch (method) {
        case aegir::volume::kMethodRead:
            answer_read(volume, words, count, badge);
            break;
        case aegir::volume::kMethodList:
            answer_list(volume, words, count, badge);
            break;
        case aegir::volume::kMethodStat:
            answer_stat(volume, words, count, badge);
            break;
        case aegir::volume::kMethodSpace:
            answer_space(volume, words, count, badge);
            break;
        case aegir::volume::kMethodOpen:
            answer_open(volume, words, count, badge);
            break;
        case aegir::volume::kMethodWrite:
            answer_write(volume, words, count, badge);
            break;
        case aegir::volume::kMethodClose:
            answer_close(volume, words, count, badge);
            break;
        case aegir::volume::kMethodMkdir:
            answer_mkdir(volume, words, count, badge);
            break;
        case aegir::volume::kMethodRemove:
            answer_remove(volume, words, count, badge);
            break;
        case aegir::volume::kMethodRename:
            answer_rename(volume, words, count, badge);
            break;
        case aegir::volume::kMethodTruncate:
            answer_truncate(volume, words, count, badge);
            break;
        case aegir::volume::kMethodReap:
            answer_reap(volume, words, count, badge);
            break;
        default:
            /* A method this version does not know is answered by saying
             * nothing (specs/services.md's versioning rule). */
            volume.reply_words(nullptr, 0);
            break;
        }
    }
}
