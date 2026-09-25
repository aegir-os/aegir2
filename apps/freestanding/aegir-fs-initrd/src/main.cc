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
                        seL4_CapRights_new(1, 0, 0, 1), 0) != seL4_NoError) {
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
    write("  initrd: ");
    write(assigned, assigned_length);
    write(": registered, serving\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        uint32_t const method =
            port.receive_words(words, aegir::ipc::kMaxWords, &count, nullptr);
        switch (method) {
        case aegir::volume::kMethodRead:
            answer_read(port, words, count);
            break;
        case aegir::volume::kMethodList:
            answer_list(port, words, count);
            break;
        case aegir::volume::kMethodStat:
            answer_stat(port, words, count);
            break;
        default:
            /* A method this version does not know is answered by saying
             * nothing (specs/services.md's versioning rule). */
            port.reply_words(nullptr, 0);
            break;
        }
    }
}
