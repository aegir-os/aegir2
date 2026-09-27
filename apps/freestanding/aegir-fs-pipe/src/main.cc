/*
 * The pipe service: the PIPE: volume -- named byte pipes (specs/pipe.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A pipe is a device, not a disk: it registers PIPE: with the VFS the way
 * NIL: does (specs/vfs.md, specs/boot.md) and serves the volume protocol
 * (aegir/volume.h) from its own memory. A name is a pipe. Opening a name for
 * writing creates it; opening it for reading attaches to it. The writer
 * appends and the reader reads at an offset; a read that reaches the current
 * end answers "no data yet" while the writer holds its end and end-of-file
 * once it has closed. The buffer grows on demand from the memory grant,
 * carved into chunks from a free list that spans it.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/nmspace.h>
#include <aegir/volume.h>
#include <sel4/sel4.h>

namespace {

void write(char const *text) noexcept
{
    aegir::debug_write(text);
}

void write(char const *text, uint32_t length) noexcept
{
    aegir::debug_write(text, length);
}

/* A pipe's name: the rest of the path after the colon. The service's own
 * bound, like the namespace's kNameMax; a longer name is refused, not
 * truncated. */
constexpr uint32_t kPipeNameMax = 32;

/* The buffer's unit. Each chunk reserves its last four bytes for the link to
 * the next chunk, so a chunk carries kChunkData bytes of pipe. */
constexpr uint32_t kChunkBytes = 4096;
constexpr uint32_t kChunkData = kChunkBytes - sizeof(uint32_t);

/* Handle kinds: a read handle attaches to a pipe's read end, a write handle
 * its write end. */
constexpr uint8_t kHandleRead = 0;
constexpr uint8_t kHandleWrite = 1;

struct Pipe {
    bool used;
    bool writer_open;
    bool reader_open;
    bool reader_ever;
    uint32_t open_count;
    uint64_t size;        /* bytes held */
    uint32_t first_chunk; /* index+1, 0 when empty */
    uint32_t last_chunk;  /* index+1, 0 when empty */
    uint32_t last_used;   /* bytes used in the last chunk, <= kChunkData */
    uint32_t name_length;
    char name[kPipeNameMax];
};

struct Handle {
    uint64_t serial;
    uint64_t badge;
    uint32_t pipe; /* index+1, 0 when free */
    uint8_t kind;
};

uint8_t *g_memory = nullptr;
uint32_t g_memory_bytes = 0;
Pipe *g_pipes = nullptr;
uint32_t g_pipe_capacity = 0;
Handle *g_handles = nullptr;
uint32_t g_handle_capacity = 0;
uint8_t *g_chunks = nullptr;
uint32_t g_chunk_count = 0;
uint32_t g_free_head = 0; /* index+1 of the first free chunk, 0 when none */
uint64_t g_handle_serial = 0;

/* The grant, divided: a quarter is the pipe and handle tables, three
 * quarters the buffers. Both scale with the grant, so a larger grant is more
 * pipes, more handles and a larger ceiling for every pipe. */
void memory_init(uint8_t *base, uint32_t bytes) noexcept
{
    g_memory = base;
    g_memory_bytes = bytes;
    for (uint32_t i = 0; i < bytes; ++i) {
        base[i] = 0;
    }
    uint32_t const meta_bytes = bytes / 4;
    g_pipes = reinterpret_cast<Pipe *>(base);
    g_pipe_capacity = (meta_bytes / 2) / static_cast<uint32_t>(sizeof(Pipe));
    g_handles = reinterpret_cast<Handle *>(base + g_pipe_capacity * sizeof(Pipe));
    g_handle_capacity =
        (meta_bytes - g_pipe_capacity * static_cast<uint32_t>(sizeof(Pipe))) /
        static_cast<uint32_t>(sizeof(Handle));
    g_chunks = base + meta_bytes;
    g_chunk_count = (bytes - meta_bytes) / kChunkBytes;
    g_free_head = 0;
    for (uint32_t i = g_chunk_count; i-- > 0;) {
        *reinterpret_cast<uint32_t *>(g_chunks + i * kChunkBytes) = g_free_head;
        g_free_head = i + 1;
    }
}

uint8_t *chunk_data(uint32_t index1) noexcept
{
    return g_chunks + (index1 - 1) * kChunkBytes;
}

uint32_t chunk_alloc() noexcept
{
    if (g_free_head == 0) {
        return 0;
    }
    uint32_t const index = g_free_head - 1;
    g_free_head = *reinterpret_cast<uint32_t *>(g_chunks + index * kChunkBytes);
    return index + 1;
}

void chunk_free(uint32_t index1) noexcept
{
    uint32_t const index = index1 - 1;
    *reinterpret_cast<uint32_t *>(g_chunks + index * kChunkBytes) = g_free_head;
    g_free_head = index1;
}

/* The next chunk in a chain, stored in the chunk's link slot. */
uint32_t chunk_next(uint32_t index1) noexcept
{
    uint32_t next = 0;
    for (uint32_t i = 0; i < sizeof(next); ++i) {
        reinterpret_cast<uint8_t *>(&next)[i] = chunk_data(index1)[kChunkData + i];
    }
    return next;
}

void chunk_set_next(uint32_t index1, uint32_t next) noexcept
{
    for (uint32_t i = 0; i < sizeof(next); ++i) {
        chunk_data(index1)[kChunkData + i] = reinterpret_cast<uint8_t const *>(&next)[i];
    }
}

Pipe *pipe_find(char const *name, uint32_t length) noexcept
{
    if (length == 0 || length > kPipeNameMax) {
        return nullptr;
    }
    for (uint32_t i = 0; i < g_pipe_capacity; ++i) {
        if (g_pipes[i].used && g_pipes[i].name_length == length) {
            bool same = true;
            for (uint32_t c = 0; same && c < length; ++c) {
                same = g_pipes[i].name[c] == name[c];
            }
            if (same) {
                return &g_pipes[i];
            }
        }
    }
    return nullptr;
}

/* The pipe `name`, made when absent: a pipe is created on first open, whether
 * the open is for reading or writing (a reader that starts first is not
 * refused -- its reads wait). Null when there is no room for another pipe. */
Pipe *pipe_make(char const *name, uint32_t length) noexcept
{
    if (length == 0 || length > kPipeNameMax) {
        return nullptr;
    }
    Pipe *found = pipe_find(name, length);
    if (found != nullptr) {
        return found;
    }
    for (uint32_t i = 0; i < g_pipe_capacity; ++i) {
        if (!g_pipes[i].used) {
            Pipe &pipe = g_pipes[i];
            pipe.used = true;
            pipe.writer_open = false;
            pipe.reader_open = false;
            pipe.reader_ever = false;
            pipe.open_count = 0;
            pipe.size = 0;
            pipe.first_chunk = 0;
            pipe.last_chunk = 0;
            pipe.last_used = 0;
            pipe.name_length = length;
            for (uint32_t c = 0; c < length; ++c) {
                pipe.name[c] = name[c];
            }
            return &pipe;
        }
    }
    return nullptr;
}

void pipe_release(Pipe &pipe) noexcept
{
    uint32_t chunk = pipe.first_chunk;
    while (chunk != 0) {
        uint32_t const next = chunk_next(chunk);
        chunk_free(chunk);
        chunk = next;
    }
    pipe.used = false;
    pipe.first_chunk = 0;
    pipe.last_chunk = 0;
    pipe.last_used = 0;
    pipe.size = 0;
    pipe.name_length = 0;
}

/* Append `count` bytes; the answer is how many the buffer could take (less
 * than asked when the grant is full, and the runtime writes the rest later). */
uint32_t pipe_append(Pipe &pipe, uint8_t const *data, uint32_t count) noexcept
{
    uint32_t taken = 0;
    while (taken < count) {
        if (pipe.last_chunk == 0) {
            uint32_t const chunk = chunk_alloc();
            if (chunk == 0) {
                break;
            }
            chunk_set_next(chunk, 0);
            pipe.first_chunk = chunk;
            pipe.last_chunk = chunk;
            pipe.last_used = 0;
        } else if (pipe.last_used == kChunkData) {
            uint32_t const chunk = chunk_alloc();
            if (chunk == 0) {
                break;
            }
            chunk_set_next(chunk, 0);
            chunk_set_next(pipe.last_chunk, chunk);
            pipe.last_chunk = chunk;
            pipe.last_used = 0;
        }
        uint32_t const room = kChunkData - pipe.last_used;
        uint32_t const take = count - taken < room ? count - taken : room;
        uint8_t *const dest = chunk_data(pipe.last_chunk) + pipe.last_used;
        for (uint32_t i = 0; i < take; ++i) {
            dest[i] = data[taken + i];
        }
        pipe.last_used += take;
        pipe.size += take;
        taken += take;
    }
    return taken;
}

/* Copy up to `count` bytes at `offset`. */
uint32_t pipe_read_at(Pipe const &pipe, uint64_t offset, uint8_t *out,
                      uint32_t count) noexcept
{
    if (offset >= pipe.size) {
        return 0;
    }
    uint32_t chunk = pipe.first_chunk;
    uint64_t position = 0;
    while (chunk != 0) {
        uint64_t const chunk_bytes =
            chunk == pipe.last_chunk ? pipe.last_used : kChunkData;
        if (offset < position + chunk_bytes) {
            break;
        }
        position += chunk_bytes;
        chunk = chunk_next(chunk);
    }
    uint32_t copied = 0;
    while (copied < count && chunk != 0) {
        uint64_t const chunk_bytes =
            chunk == pipe.last_chunk ? pipe.last_used : kChunkData;
        uint64_t const within = offset + copied - position;
        if (within >= chunk_bytes) {
            position += chunk_bytes;
            chunk = chunk_next(chunk);
            continue;
        }
        uint64_t const room = chunk_bytes - within;
        uint64_t const take = count - copied < room ? count - copied : room;
        uint8_t const *const source = chunk_data(chunk) + within;
        for (uint64_t i = 0; i < take; ++i) {
            out[copied + i] = source[i];
        }
        copied += static_cast<uint32_t>(take);
    }
    return copied;
}

Handle *handle_lookup(uint64_t serial, uint64_t badge) noexcept
{
    if (serial == 0) {
        return nullptr;
    }
    for (uint32_t i = 0; i < g_handle_capacity; ++i) {
        if (g_handles[i].serial == serial) {
            return g_handles[i].badge == badge ? &g_handles[i] : nullptr;
        }
    }
    return nullptr;
}

Handle *handle_alloc(uint64_t badge) noexcept
{
    for (uint32_t i = 0; i < g_handle_capacity; ++i) {
        if (g_handles[i].serial == 0) {
            g_handles[i].serial = ++g_handle_serial;
            g_handles[i].badge = badge;
            return &g_handles[i];
        }
    }
    return nullptr;
}

/* read: a path, an offset, a count. A one-shot path read is served the same
 * way a handle read is, against the pipe the path names. */
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
    Pipe *pipe = pipe_find(path, path_length);
    if (pipe == nullptr) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const offset = words[path_words];
    uint64_t wanted = words[path_words + 1];
    if (wanted > aegir::volume::kReadMax) {
        wanted = aegir::volume::kReadMax;
    }
    uint8_t bytes[aegir::volume::kReadMax];
    uint32_t const got = pipe_read_at(*pipe, offset, bytes, static_cast<uint32_t>(wanted));
    uint64_t answer[aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8];
    answer[0] = got;
    answer[1] = (offset + got >= pipe->size && !pipe->writer_open) ? 1 : 0;
    uint8_t *const packed = reinterpret_cast<uint8_t *>(answer + aegir::volume::kReadHeaderWords);
    for (uint32_t i = 0; i < got; ++i) {
        packed[i] = bytes[i];
    }
    port.reply_words(answer, aegir::volume::kReadHeaderWords +
                                 static_cast<uint32_t>((got + 7) / 8));
}

void answer_read_handle(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                        uint64_t badge) noexcept
{
    if (count < 3) {
        port.reply_words(nullptr, 0);
        return;
    }
    Handle *handle = handle_lookup(words[0], badge);
    if (handle == nullptr || handle->kind != kHandleRead || handle->pipe == 0) {
        port.reply_words(nullptr, 0);
        return;
    }
    Pipe &pipe = g_pipes[handle->pipe - 1];
    uint64_t const offset = words[1];
    uint64_t wanted = words[2];
    if (wanted > aegir::volume::kReadMax) {
        wanted = aegir::volume::kReadMax;
    }
    uint8_t bytes[aegir::volume::kReadMax];
    uint32_t const got = pipe_read_at(pipe, offset, bytes, static_cast<uint32_t>(wanted));
    uint64_t answer[aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8];
    answer[0] = got;
    /* No data yet while the writer holds its end; end-of-file once it has
     * closed and the read has reached the end (specs/pipe.md). */
    answer[1] = (offset + got >= pipe.size && !pipe.writer_open) ? 1 : 0;
    uint8_t *const packed = reinterpret_cast<uint8_t *>(answer + aegir::volume::kReadHeaderWords);
    for (uint32_t i = 0; i < got; ++i) {
        packed[i] = bytes[i];
    }
    port.reply_words(answer, aegir::volume::kReadHeaderWords +
                                 static_cast<uint32_t>((got + 7) / 8));
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
    if (handle == nullptr || handle->kind != kHandleWrite || handle->pipe == 0 ||
        bytes > aegir::volume::kWriteMax ||
        count < 2 + static_cast<uint32_t>((bytes + 7) / 8)) {
        port.reply_words(&written, 1);
        return;
    }
    Pipe &pipe = g_pipes[handle->pipe - 1];
    auto const *data = reinterpret_cast<uint8_t const *>(words + 2);
    written = pipe_append(pipe, data, static_cast<uint32_t>(bytes));
    port.reply_words(&written, 1);
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
    if (count < path_words + 1) {
        port.reply_words(&handle, 1);
        return;
    }
    uint64_t const flags = words[path_words];
    bool const reading = (flags & aegir::volume::kOpenRead) != 0;
    Pipe *pipe = pipe_make(path, path_length);
    if (pipe == nullptr) {
        port.reply_words(&handle, 1);
        return;
    }
    if (!reading && (flags & aegir::volume::kOpenTruncate) != 0) {
        /* A write open cuts what a stale name held: a reused pipe is a new
         * one (specs/pipe.md). */
        uint32_t chunk = pipe->first_chunk;
        while (chunk != 0) {
            uint32_t const next = chunk_next(chunk);
            chunk_free(chunk);
            chunk = next;
        }
        pipe->first_chunk = 0;
        pipe->last_chunk = 0;
        pipe->last_used = 0;
        pipe->size = 0;
    }
    Handle *row = handle_alloc(badge);
    if (row != nullptr) {
        row->pipe = static_cast<uint32_t>(pipe - g_pipes) + 1;
        row->kind = reading ? kHandleRead : kHandleWrite;
        handle = row->serial;
        ++pipe->open_count;
        if (reading) {
            pipe->reader_open = true;
            pipe->reader_ever = true;
        } else {
            pipe->writer_open = true;
        }
    }
    port.reply_words(&handle, 1);
}

void answer_close(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                  uint64_t badge) noexcept
{
    uint64_t closed = 0;
    if (count >= 1) {
        Handle *handle = handle_lookup(words[0], badge);
        if (handle != nullptr && handle->pipe != 0) {
            Pipe &pipe = g_pipes[handle->pipe - 1];
            if (handle->kind == kHandleRead) {
                pipe.reader_open = false;
            } else {
                pipe.writer_open = false;
            }
            if (pipe.open_count != 0) {
                --pipe.open_count;
            }
            handle->serial = 0;
            handle->pipe = 0;
            /* The pipe dies when its reader has used it and no handle is
             * left; a writer that closes before any reader keeps the pipe
             * (and its bytes) for the reader still to come. */
            if (pipe.reader_ever && pipe.open_count == 0) {
                pipe_release(pipe);
            }
            closed = 1;
        }
    }
    port.reply_words(&closed, 1);
}

/* stat: every name is a file that is there, so an open reaches it (the NIL:
 * rule, specs/pipe.md); the root is a directory. */
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
    uint64_t answer[aegir::volume::kStatTailWords] = {
        path_length == 0 ? aegir::volume::kKindDir : aegir::volume::kKindFile, 0, 0};
    port.reply_words(answer, aegir::volume::kStatTailWords);
}

void answer_refuse(aegir::ipc::Owner &port) noexcept
{
    uint64_t const no = 0;
    port.reply_words(&no, 1);
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

    aegir::ipc::Owner port = aegir::ipc::Owner::find("vol.pipe", 8);
    aegir::ipc::Consumer const nmspace =
        aegir::ipc::Consumer::find(aegir::nmspace::kPortName, aegir::nmspace::kPortNameLength);
    if (!port.valid() || !nmspace.valid()) {
        write("  pipe: no port or no namespace\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The buffers and the tables live in the memory grant. */
    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    if (!aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address) ||
        memory_bits == 0 || memory_address == 0) {
        write("  pipe: no memory grant for the buffers\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    memory_init(reinterpret_cast<uint8_t *>(memory_address), 1u << memory_bits);

    /* The volume's caller half, minted unbadged: the VFS badges each
     * resolver's own copy, which a badged cap would make impossible. */
    uint64_t owner_slot = 0;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            if (entry.kind == aegir::bootstrap::EntryKind::Capability &&
                entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            }
        }
    }
    static_cast<void>(aegir::bootstrap::capability("vol.pipe", 8, &owner_slot));
    seL4_CPtr const caller_half = static_cast<seL4_CPtr>(first_free);
    if (owner_slot == 0 ||
        seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, caller_half,
                        aegir::bootstrap::kCNodeBits, aegir::bootstrap::kSlotOwnCNode,
                        static_cast<seL4_CPtr>(owner_slot), aegir::bootstrap::kCNodeBits,
                        seL4_CapRights_new(1, 0, 0, 1), 0) != seL4_NoError) {
        write("  pipe: the caller half would not mint\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* Register, then serve. PIPE is public, like NIL: a session and the
     * commands it starts resolve PIPE:name through the namespace. */
    constexpr char kVolume[] = "PIPE";
    uint64_t out[aegir::nmspace::kNameMax / 8 + aegir::nmspace::kTypeMax / 8 + 2];
    uint32_t out_words = aegir::nmspace::pack_string(out, kVolume, sizeof(kVolume) - 1,
                                                     aegir::nmspace::kNameMax);
    out[out_words++] = aegir::nmspace::kFlagPublic;
    out_words += aegir::nmspace::pack_string(out + out_words, "PIPE", 4,
                                             aegir::nmspace::kTypeMax);
    uint64_t in[aegir::nmspace::kNameMax / 8 + 1];
    aegir::ipc::WordsReply const registered =
        nmspace.call_transfer(aegir::nmspace::kMethodRegister, out, out_words, caller_half,
                              in, aegir::nmspace::kNameMax / 8 + 1, nullptr);
    /* The kernel transferred a copy; our half of the mint leaves. */
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, caller_half,
                      aegir::bootstrap::kCNodeBits);
    char const *assigned = nullptr;
    uint32_t assigned_length = 0;
    if (registered.error != 0 || registered.count == 0 ||
        !aegir::nmspace::unpack_string(in, registered.count, aegir::nmspace::kNameMax,
                                       &assigned, &assigned_length)) {
        write("  pipe: the namespace refused the registration\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    write("  pipe: ");
    write(assigned, assigned_length);
    write(": registered, serving\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        seL4_Word badge = 0;
        uint32_t const method =
            port.receive_words(words, aegir::ipc::kMaxWords, &count, &badge);
        switch (method) {
        case aegir::volume::kMethodRead:
            answer_read(port, words, count);
            break;
        case aegir::volume::kMethodReadHandle:
            answer_read_handle(port, words, count, badge);
            break;
        case aegir::volume::kMethodWrite:
            answer_write(port, words, count, badge);
            break;
        case aegir::volume::kMethodOpen:
            answer_open(port, words, count, badge);
            break;
        case aegir::volume::kMethodClose:
            answer_close(port, words, count, badge);
            break;
        case aegir::volume::kMethodStat:
            answer_stat(port, words, count);
            break;
        case aegir::volume::kMethodList:
            /* A pipe has no entries: an empty reply is the end of the
             * (empty) directory. */
            port.reply_words(nullptr, 0);
            break;
        case aegir::volume::kMethodMkdir:
        case aegir::volume::kMethodRemove:
        case aegir::volume::kMethodReap:
        case aegir::volume::kMethodRename:
        case aegir::volume::kMethodTruncate:
            answer_refuse(port);
            break;
        default:
            /* A method this version does not know is answered by saying
             * nothing (specs/services.md's versioning rule). */
            port.reply_words(nullptr, 0);
            break;
        }
    }
}
