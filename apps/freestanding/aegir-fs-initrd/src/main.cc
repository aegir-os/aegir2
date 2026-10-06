/*
 * The initrd service: the Initrd: volume -- the boot image, served read-only.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The flat archive director starts services from is also a filesystem: no
 * directories, entry names that are identities (specs/services.md). This
 * service is that archive as a volume (specs/vfs.md): it is handed the
 * archive mapped read-only (the manifest's `initrd` field -- the same
 * mapping a spawner gets, without the spawn authority), registers Initrd:
 * with the VFS itself, and answers the volume protocol (aegir/volume.h)
 * from the archive in place, through libcpio.
 *
 * Registering itself means minting the unbadged caller half the
 * registration carries -- the port graph gives this service's owner half
 * the rights that takes (aegir-director's ports.cc says why). A flat
 * filesystem has no paths: a name with a `/` in it simply does not exist,
 * and case is exact, because the archive's names are.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/nmspace.h>
#include <aegir/volume.h>

extern "C" {
/* libcpio is C; the boundary is handled the way director's own use of it is. */
#include <cpio/cpio.h>
}
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

void const *g_archive = nullptr;
uint64_t g_archive_bytes = 0;
uint32_t g_name_max = 0;

/* The bulk-read window (specs/vfs.md's scaling path): the `maps` grant gives
 * this service its own VSpace root and a window of free addresses, and the
 * page tables over that window are retyped out of the untyped the same grant
 * carries. A read-frame call maps the caller's frame at `g_window_base`, copies
 * the file's bytes into it, and unmaps it -- one call per 4 KiB rather than five
 * of the inline read it replaces, and the bytes never cross a message. */
aegir::mem::Allocator g_tables(nullptr);
aegir::mem::Scratch g_window(nullptr);
uintptr_t g_window_base = 0;
bool g_window_ready = false;

/* The length of an entry's name: the archive's names are NUL-terminated on
 * disk (cpio_get_file compares against one, projects/util_libs/libcpio/
 * src/cpio.c:209), and the scan stays inside the archive's own bound. */
uint32_t name_length(char const *name) noexcept
{
    for (uint32_t i = 0; i < g_name_max; ++i) {
        if (name[i] == '\0') {
            return i;
        }
    }
    return g_name_max;
}

/* The entry named `name`/`length`, or nothing: its data and size. The scan
 * is the archive's own order, one entry at a time. */
void const *find_entry(char const *name, uint32_t length, uint64_t *size) noexcept
{
    for (int i = 0;; ++i) {
        char const *entry_name = nullptr;
        unsigned long entry_size = 0;
        void const *data =
            cpio_get_entry(g_archive, g_archive_bytes, i, &entry_name, &entry_size);
        if (data == nullptr) {
            return nullptr;
        }
        uint32_t const entry_length = name_length(entry_name);
        if (entry_length != length) {
            continue;
        }
        bool same = true;
        for (uint32_t c = 0; same && c < length; ++c) {
            same = entry_name[c] == name[c];
        }
        if (same) {
            *size = entry_size;
            return data;
        }
    }
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
    uint64_t size = 0;
    void const *data = find_entry(path, path_length, &size);
    if (data == nullptr || offset > size) {
        port.reply_words(nullptr, 0);
        return;
    }
    if (wanted > aegir::volume::kReadMax) {
        wanted = aegir::volume::kReadMax;
    }
    uint64_t const available = size - offset;
    uint64_t const got = wanted < available ? wanted : available;
    uint64_t answer[aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8];
    answer[0] = got;
    answer[1] = offset + got >= size ? 1 : 0;
    char *bytes = reinterpret_cast<char *>(answer + aegir::volume::kReadHeaderWords);
    char const *from = static_cast<char const *>(data) + offset;
    for (uint64_t i = 0; i < got; ++i) {
        bytes[i] = from[i];
    }
    port.reply_words(answer, aegir::volume::kReadHeaderWords +
                                 static_cast<uint32_t>((got + 7) / 8));
}

/* A read handle, packed: the file's data offset in the archive in the high bits
 * and its size in the low. The archive is read-only and its layout fixed, so
 * the handle needs no table and no per-client state -- a read-handle decodes it
 * and copies in place, which is what makes a caller reading a program image a
 * window at a time cheap. `kHandleSizeBits` bounds a file at 16 MiB and an offset
 * at 1 TiB, both far past the boot image (specs/vfs.md). */
constexpr uint32_t kHandleSizeBits = 24;

void answer_open(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
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
    if (count < path_words + 1 || (words[path_words] & aegir::volume::kOpenRead) == 0) {
        /* Read-only: a write open is refused, and so is a malformed call. */
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t size = 0;
    void const *data = find_entry(path, path_length, &size);
    if (data == nullptr || size >= (1ull << kHandleSizeBits)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const offset = static_cast<uint64_t>(
        static_cast<char const *>(data) - static_cast<char const *>(g_archive));
    uint64_t const handle = (offset << kHandleSizeBits) | size;
    uint64_t const answer[1] = {handle};
    port.reply_words(answer, 1);
}

void answer_read_handle(aegir::ipc::Owner &port, uint64_t const *words,
                        uint32_t count) noexcept
{
    if (count < 3) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const handle = words[0];
    uint64_t const offset = words[1];
    uint64_t wanted = words[2];
    uint64_t const file_offset = handle >> kHandleSizeBits;
    uint64_t const size = handle & ((1ull << kHandleSizeBits) - 1);
    if (handle == 0 || file_offset + size > g_archive_bytes || offset > size) {
        port.reply_words(nullptr, 0);
        return;
    }
    if (wanted > aegir::volume::kReadMax) {
        wanted = aegir::volume::kReadMax;
    }
    uint64_t const available = size - offset;
    uint64_t const got = wanted < available ? wanted : available;
    uint64_t answer[aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8];
    answer[0] = got;
    answer[1] = offset + got >= size ? 1 : 0;
    char *bytes = reinterpret_cast<char *>(answer + aegir::volume::kReadHeaderWords);
    char const *from = static_cast<char const *>(g_archive) + file_offset + offset;
    for (uint64_t i = 0; i < got; ++i) {
        bytes[i] = from[i];
    }
    port.reply_words(answer, aegir::volume::kReadHeaderWords +
                                 static_cast<uint32_t>((got + 7) / 8));
}

/* read-frame: the bulk form of a read. A handle, an offset in the file, how
 * many bytes, how far into the frame; and one capability beside the words, the
 * caller's own 4 KiB frame. The frame is mapped into our window, the bytes are
 * copied into it at that offset, and it is unmapped again before we answer --
 * the mapping does not outlive the call, and the frame is the caller's own
 * rather than a window shared between clients (aegir/block.h's caveat).
 * The answer is read's header alone. */
void answer_read_frame(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                       bool cap_arrived) noexcept
{
    if (count < 4 || !cap_arrived || !g_window_ready) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                          aegir::bootstrap::kSlotReceiveCap, aegir::bootstrap::kCNodeBits);
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const handle = words[0];
    uint64_t const offset = words[1];
    uint64_t wanted = words[2];
    uint64_t const frame_offset = words[3];
    uint64_t const file_offset = handle >> kHandleSizeBits;
    uint64_t const size = handle & ((1ull << kHandleSizeBits) - 1);
    uint32_t const frame_bits =
        count >= 5 ? static_cast<uint32_t>(words[4]) : aegir::volume::kFrameBitsMin;
    if (frame_bits < aegir::volume::kFrameBitsMin ||
        frame_bits > aegir::volume::kFrameBitsMax) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                          aegir::bootstrap::kSlotReceiveCap, aegir::bootstrap::kCNodeBits);
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const frame_size = 1ull << frame_bits;
    if (handle == 0 || file_offset + size > g_archive_bytes || offset > size ||
        frame_offset >= frame_size) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                          aegir::bootstrap::kSlotReceiveCap, aegir::bootstrap::kCNodeBits);
        port.reply_words(nullptr, 0);
        return;
    }
    /* The frame cap sits in the scratch receive slot; map it from there, not
     * through a slot of our own -- the map is what names it, and a move first
     * would be one more kernel call per page. The slot is emptied after. */
    seL4_CPtr const frame = aegir::bootstrap::kSlotReceiveCap;
    uintptr_t address = g_window_base;
    if (frame_bits > aegir::volume::kFrameBitsMin) {
        void *const window = g_window.map_large(frame);
        if (window == nullptr) {
            seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, frame,
                              aegir::bootstrap::kCNodeBits);
            port.reply_words(nullptr, 0);
            return;
        }
        address = reinterpret_cast<uintptr_t>(window);
    } else if (!g_window.map_at(g_window_base, frame)) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, frame,
                          aegir::bootstrap::kCNodeBits);
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const room = frame_size - frame_offset;
    if (wanted > room) {
        wanted = room;
    }
    uint64_t const available = size - offset;
    uint64_t const got = wanted < available ? wanted : available;
    __builtin_memcpy(reinterpret_cast<char *>(address) + frame_offset,
                     static_cast<char const *>(g_archive) + file_offset + offset, got);
    g_window.unmap(frame);
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, frame, aegir::bootstrap::kCNodeBits);
    uint64_t const answer[aegir::volume::kReadHeaderWords] = {
        got, offset + got >= size ? 1ULL : 0ULL};
    port.reply_words(answer, aegir::volume::kReadHeaderWords);
}

void answer_close(aegir::ipc::Owner &port) noexcept
{
    /* The handle is stateless, so a close has nothing to free and answers yes. */
    uint64_t const answer[1] = {1};
    port.reply_words(answer, 1);
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
    /* A flat filesystem has exactly one directory: the volume's root, named
     * by the empty rest. Anything else is not a directory here. */
    if (path_length != 0) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint32_t const path_words = 1;
    if (count < path_words + 1) {
        port.reply_words(nullptr, 0);
        return;
    }
    char const *entry_name = nullptr;
    unsigned long entry_size = 0;
    void const *data = cpio_get_entry(g_archive, g_archive_bytes,
                                      static_cast<int>(words[path_words]), &entry_name,
                                      &entry_size);
    if (data == nullptr) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint32_t const entry_length = name_length(entry_name);
    uint64_t answer[aegir::ipc::kMaxWords];
    uint32_t const name_words =
        aegir::nmspace::pack_string(answer, entry_name, entry_length, g_name_max);
    if (name_words == 0 || name_words + aegir::volume::kListTailWords > aegir::ipc::kMaxWords) {
        port.reply_words(nullptr, 0);
        return;
    }
    answer[name_words] = entry_size;
    answer[name_words + 1] = aegir::volume::kKindFile;
    /* The archive carries no times, as its stat answers zero. */
    answer[name_words + 2] = 0;
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
    uint64_t kind = 0;
    uint64_t size = 0;
    if (path_length == 0) {
        /* The root is the archive itself: a directory with no size. */
        kind = aegir::volume::kKindDir;
    } else {
        uint64_t entry_size = 0;
        if (find_entry(path, path_length, &entry_size) == nullptr) {
            port.reply_words(nullptr, 0);
            return;
        }
        kind = aegir::volume::kKindFile;
        size = entry_size;
    }
    uint64_t answer[aegir::volume::kStatTailWords] = {kind, size};
    port.reply_words(answer, aegir::volume::kStatTailWords);
}

void answer_space(aegir::ipc::Owner &port) noexcept
{
    /* The archive's size, and no free room: the boot image is read-only, so
     * there is nowhere for a write to land (specs/vfs.md). */
    uint64_t const answer[aegir::volume::kSpaceTailWords] = {g_archive_bytes, 0};
    port.reply_words(answer, aegir::volume::kSpaceTailWords);
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

    uint64_t archive_address = 0;
    uint32_t archive_bytes = 0;
    aegir::ipc::Owner port = aegir::ipc::Owner::find("vol.initrd", 10);
    aegir::ipc::Consumer const nmspace =
        aegir::ipc::Consumer::find(aegir::nmspace::kPortName, aegir::nmspace::kPortNameLength);
    struct cpio_info info {};
    if (!aegir::bootstrap::binaries(&archive_address, &archive_bytes) ||
        !port.valid() || !nmspace.valid()) {
        write("  initrd: no archive, no port, or no namespace\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    g_archive = reinterpret_cast<void const *>(archive_address);
    g_archive_bytes = archive_bytes;
    if (cpio_info(g_archive, g_archive_bytes, &info) != 0) {
        write("  initrd: the archive I was given cannot be read\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    g_name_max = info.max_path_sz;

    /* The volume's caller half, minted unbadged: the VFS badges each
     * resolver's own copy, which a badged cap would make impossible. The
     * slots past the block's names are ours. */
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
    static_cast<void>(aegir::bootstrap::capability("vol.initrd", 10, &owner_slot));
    seL4_CPtr const caller_half = static_cast<seL4_CPtr>(first_free);
    if (owner_slot == 0 ||
        seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, caller_half,
                        aegir::bootstrap::kCNodeBits, aegir::bootstrap::kSlotOwnCNode,
                        static_cast<seL4_CPtr>(owner_slot), aegir::bootstrap::kCNodeBits,
                        seL4_CapRights_new(1, 1, 0, 1), 0) != seL4_NoError) {
        write("  initrd: the caller half would not mint\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* Register, then serve: a volume nobody can find is not one. The answer
     * is the name the volume actually got -- Initrd has no label to
     * discover, so the name asked for is the name expected. */
    constexpr char kVolume[] = "Initrd";
    uint64_t out[aegir::nmspace::kNameMax / 8 + aegir::nmspace::kTypeMax / 8 + 2];
    uint32_t out_words = aegir::nmspace::pack_string(out, kVolume, sizeof(kVolume) - 1,
                                                     aegir::nmspace::kNameMax);
    out[out_words++] = aegir::nmspace::kFlagReadOnly | aegir::nmspace::kFlagPublic;
    /* The initrd's own type, beside the flags; it is not a BFS or FAT volume
     * (specs/vfs.md). */
    out_words += aegir::nmspace::pack_string(out + out_words, "INITRD", 6,
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
        write("  initrd: the namespace refused the registration\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The bulk-read window (specs/vfs.md's scaling path). The maps grant gives
     * us a VSpace root and a window of free addresses; the page tables over it
     * are retyped from the untyped the same grant carries, and the first
     * read-frame builds them -- the same FailedLookup idiom a spawner's window
     * uses. One page at the window's base is reserved for the caller's frame, so
     * the allocator's own node frames (which map at the cursor) never land on
     * it. A grant that is absent or short leaves bulk reads off; inline reads
     * are unchanged. `caller_half` was deleted above, so `first_free` is free
     * again and the allocator takes those slots. */
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    uint64_t untyped_slot = 0;
    uint32_t untyped_bits = 0;
    if (aegir::bootstrap::capability("vspace", 6, &vspace_slot) &&
        aegir::bootstrap::window(&window_base, &window_bytes) &&
        aegir::bootstrap::capability("untyped", 7, &untyped_slot) &&
        aegir::bootstrap::capability_size_bits("untyped", 7, &untyped_bits)) {
        g_tables.adopt_slots(
            static_cast<seL4_CPtr>(first_free),
            (1u << aegir::bootstrap::kCNodeBits) - static_cast<uint32_t>(first_free), 0,
            aegir::bootstrap::kCNodeBits);
        static_cast<void>(g_tables.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot),
                                                 untyped_bits));
        g_window_base = static_cast<uintptr_t>(window_base);
        if (g_window.adopt(static_cast<seL4_CPtr>(vspace_slot), g_window_base,
                           g_window_base + window_bytes, &g_tables)) {
            uintptr_t const reserved = g_window.reserve(1);
            if (reserved != 0) {
                g_window_base = reserved;
                g_window_ready = true;
            }
        }
    }
    if (!g_window_ready) {
        write("  initrd: no window for bulk reads; serving inline reads\n");
    }

    write("  initrd: ");
    write(assigned, assigned_length);
    write(": registered, serving\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        bool cap_arrived = false;
        uint32_t const method =
            port.receive_words(words, aegir::ipc::kMaxWords, &count, nullptr, &cap_arrived);
        switch (method) {
        case aegir::volume::kMethodRead:
            answer_read(port, words, count);
            break;
        case aegir::volume::kMethodReadHandle:
            answer_read_handle(port, words, count);
            break;
        case aegir::volume::kMethodReadFrame:
            answer_read_frame(port, words, count, cap_arrived);
            break;
        case aegir::volume::kMethodOpen:
            answer_open(port, words, count);
            break;
        case aegir::volume::kMethodClose:
            answer_close(port);
            break;
        case aegir::volume::kMethodList:
            answer_list(port, words, count);
            break;
        case aegir::volume::kMethodStat:
            answer_stat(port, words, count);
            break;
        case aegir::volume::kMethodSpace:
            answer_space(port);
            break;
        default:
            /* A method this version does not know is answered by saying
             * nothing (specs/services.md's versioning rule). */
            port.reply_words(nullptr, 0);
            break;
        }
        /* A capability that arrived on a method that does not take one would
         * otherwise sit in the receive slot and refuse the next transfer;
         * read-frame consumed or dropped its own. */
        if (cap_arrived && method != aegir::volume::kMethodReadFrame) {
            seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                              aegir::bootstrap::kSlotReceiveCap,
                              aegir::bootstrap::kCNodeBits);
        }
    }
}
