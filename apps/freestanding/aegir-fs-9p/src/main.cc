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
 * message. It registers the export's own tag as a volume, so `list`, `type` and
 * the rest reach the host directory by the ordinary file protocol -- which is
 * the whole reason the transport exists (specs/9p.md).
 *
 * A read path is stateless: walk from the attached root, open, read, clunk. A
 * write path is a handle, and the handle side is phase 3; until then the volume
 * answers no read handle (the hosted file layer falls back to path reads) and
 * is marked read-only. What the messages *mean* is here; how they travel is the
 * transport's (aegir/p9transport.h); the machine is neither.
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

/* The maps grant: our own objects, the window we map the transport's frames
 * through, and what we charge to. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

/* The transport driver's port and the window it serves through, mapped into our
 * own address space at consecutive pages. */
aegir::ipc::Consumer g_transport;
uint8_t *g_window = nullptr;
uint32_t g_window_bytes = 0;

/* The export's name, read from the transport's `info`. */
char g_tag[aegir::p9transport::kTagMax];
uint32_t g_tag_length = 0;

/* Fids are the client's own; 0 is the attached root and is never clunked. A
 * scratch fid is allocated per call and released at the end of it, so the
 * counter only has to avoid 0 and the reserved value. */
uint32_t g_fid_next = 1;

/* The path a list entry's own getattr is walked at: the directory's path, a
 * slash, the entry's name. Static and generously sized because the service is
 * single-threaded and a deep path plus a long name can approach kPathMax. */
char g_child[aegir::nmspace::kPathMax + 256];

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

/** One chunk of components walked in a single Twalk: a path is walked a handful
 *  at a time so neither a Name array nor the message has to hold the whole
 *  path at once. Large enough that an ordinary path is one walk. */
constexpr uint32_t kWalkChunk = 16;

/** Walk `path` (slash-separated; empty is the root) from `start` into a fresh
 *  fid. Answers that fid, or kNoFid when any component is missing; `start` is
 *  left alone, and for an empty path the fresh fid is a clone of it. `last`
 *  receives the final component's qid when it is not null. */
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

/* ------------------------------------------------------------------ *
 * The volume protocol (aegir/volume.h).
 * ------------------------------------------------------------------ */

/* read: a path, an offset, how many bytes. The file is walked and opened, the
 * bytes are copied into the answer *before* the clunk (which is another
 * message and would overwrite the reply), and the clunk releases the fid. */
void answer_read(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
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
void answer_list(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
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
        aegir::p9::Client::Attr attr{};
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
void answer_stat(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
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
        aegir::p9::Client::Attr attr{};
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
void answer_space(aegir::ipc::Owner &port) noexcept
{
    aegir::p9::Client::Statfs stats{};
    if (!g_client.statfs(0, &stats)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const answer[aegir::volume::kSpaceTailWords] = {stats.total, stats.free};
    port.reply_words(answer, aegir::volume::kSpaceTailWords);
}

/* open: no read handles until the write side lands, so the answer is zero --
 * the volume's "no handle, fall back to path reads" (aegir/vfs-client's
 * Volume::open_read), not a refusal. */
void answer_open(aegir::ipc::Owner &port) noexcept
{
    uint64_t const answer[1] = {0};
    port.reply_words(answer, 1);
}

/* close: nothing was opened to a handle, so nothing is freed. */
void answer_close(aegir::ipc::Owner &port) noexcept
{
    uint64_t const answer[1] = {1};
    port.reply_words(answer, 1);
}

/* reap: no handle is held, so a badge has nothing to drop. */
void answer_reap(aegir::ipc::Owner &port) noexcept
{
    uint64_t const answer[1] = {0};
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

/* Open the bound p9.* transport and map the window it serves through into our
 * own address space. The row may not be there yet -- the device manager binds
 * the driver asynchronously -- so the caller retries. */
bool open_transport(int64_t *row_out, seL4_CPtr *port_slot_out) noexcept
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
    *row_out = row;
    *port_slot_out = port_slot;
    return true;
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
     * transport's frames through. */
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

    /* The transport, retried until the device manager has bound its driver. */
    int64_t row = -1;
    seL4_CPtr port_slot = 0;
    while (!open_transport(&row, &port_slot)) {
        seL4_Yield();
    }
    static_cast<void>(row);
    static_cast<void>(port_slot);

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
     * directory, so the whole path -- walk, open, read, clunk -- is proved
     * against the host's own bytes. */
    {
        static char const kFile[] = "hello.txt";
        char content[256];
        uint32_t got = 0;
        bool ok = false;
        uint32_t const fid = walk_path(0, kFile, sizeof(kFile) - 1, nullptr);
        if (fid != aegir::p9::kNoFid) {
            if (g_client.open(fid, aegir::p9::kORead)) {
                uint8_t const *data = nullptr;
                int32_t const n = g_client.read(fid, 0, sizeof(content), &data);
                if (n >= 0) {
                    got = static_cast<uint32_t>(n);
                    for (uint32_t i = 0; i < got; ++i) {
                        content[i] = static_cast<char>(data[i]);
                    }
                    ok = true;
                }
            }
            (void)g_client.clunk(fid);
        }
        if (!ok) {
            write("  9p: FAIL the export would not read hello.txt\n");
        } else {
            write("  9p: read ");
            write_unsigned(got);
            write(" bytes: ");
            write(content, got);
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

    /* Register the volume, named after the export. Read-only until the write
     * side lands (specs/9p.md). */
    uint64_t out[aegir::nmspace::kNameMax / 8 + aegir::nmspace::kTypeMax / 8 + 2];
    uint32_t out_words = aegir::nmspace::pack_string(out, g_tag, g_tag_length,
                                                     aegir::nmspace::kNameMax);
    out[out_words++] = aegir::nmspace::kFlagReadOnly | aegir::nmspace::kFlagPublic;
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
        uint32_t const method =
            volume.receive_words(words, aegir::ipc::kMaxWords, &count, nullptr);
        switch (method) {
        case aegir::volume::kMethodRead:
            answer_read(volume, words, count);
            break;
        case aegir::volume::kMethodList:
            answer_list(volume, words, count);
            break;
        case aegir::volume::kMethodStat:
            answer_stat(volume, words, count);
            break;
        case aegir::volume::kMethodSpace:
            answer_space(volume);
            break;
        case aegir::volume::kMethodOpen:
            answer_open(volume);
            break;
        case aegir::volume::kMethodClose:
            answer_close(volume);
            break;
        case aegir::volume::kMethodReap:
            answer_reap(volume);
            break;
        default:
            /* A method this version does not know is answered by saying
             * nothing (specs/services.md's versioning rule). */
            volume.reply_words(nullptr, 0);
            break;
        }
    }
}
