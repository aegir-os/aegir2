/*
 * The NIL: handler: the null device (specs/boot.md, specs/vfs.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The Amiga's NIL: device is where output goes to disappear and input is
 * always at its end. Aegir serves it as a volume like any other: it registers
 * NIL: with the VFS and answers the volume protocol (aegir/volume.h), so
 * redirection reaches it by path and the VFS needs no special case for it. A
 * read answers EOF at every path, a write is accepted and dropped, an open
 * always succeeds -- there is nothing to create and nothing to truncate -- and
 * mkdir, remove, rename and truncate are refused. EndCLI >NIL: is its first
 * use.
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

/* A handle's serial is never reused (specs/vfs.md); a null device has no
 * other state. */
uint64_t g_next_handle = 1;

/* read: a null device is always at its end, at every path -- the volume root
 * included, because NIL: is a device and not a directory of files. */
void answer_read(aegir::ipc::Owner &port) noexcept
{
    uint64_t answer[aegir::volume::kReadHeaderWords] = {0, 1};
    port.reply_words(answer, aegir::volume::kReadHeaderWords);
}

/* list: no entries, ever. An empty reply is "no entry at this index". */
void answer_list(aegir::ipc::Owner &port) noexcept
{
    port.reply_words(nullptr, 0);
}

/* stat: the root is a directory (the protocol's rule); every other name is a
 * device that is there, so open reaches it. */
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

/* open: every name is the null device, so every open succeeds. The mode flags
 * are ignored -- there is nothing to create and nothing to truncate. */
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
    uint64_t handle = g_next_handle++;
    port.reply_words(&handle, 1);
}

/* write: accepted and dropped. The request is the handle, then the count. */
void answer_write(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    if (count < 2) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const written = words[1];
    port.reply_words(&written, 1);
}

/* close: nothing was open. */
void answer_close(aegir::ipc::Owner &port) noexcept
{
    uint64_t const ok = 1;
    port.reply_words(&ok, 1);
}

/* The mutating methods and a directory listing's mkdir/remove: a null device
 * has nothing to change. */
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

    aegir::ipc::Owner port = aegir::ipc::Owner::find("vol.nil", 7);
    aegir::ipc::Consumer const nmspace =
        aegir::ipc::Consumer::find(aegir::nmspace::kPortName, aegir::nmspace::kPortNameLength);
    if (!port.valid() || !nmspace.valid()) {
        write("  nil: no port or no namespace\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

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
    static_cast<void>(aegir::bootstrap::capability("vol.nil", 7, &owner_slot));
    seL4_CPtr const caller_half = static_cast<seL4_CPtr>(first_free);
    if (owner_slot == 0 ||
        seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, caller_half,
                        aegir::bootstrap::kCNodeBits, aegir::bootstrap::kSlotOwnCNode,
                        static_cast<seL4_CPtr>(owner_slot), aegir::bootstrap::kCNodeBits,
                        seL4_CapRights_new(1, 0, 0, 1), 0) != seL4_NoError) {
        write("  nil: the caller half would not mint\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* Register, then serve. NIL is writable -- a write is how output
     * disappears -- and public, so every session can redirect to it. */
    constexpr char kVolume[] = "NIL";
    uint64_t out[aegir::nmspace::kNameMax / 8 + aegir::nmspace::kTypeMax / 8 + 2];
    uint32_t out_words = aegir::nmspace::pack_string(out, kVolume, sizeof(kVolume) - 1,
                                                     aegir::nmspace::kNameMax);
    out[out_words++] = aegir::nmspace::kFlagPublic;
    out_words += aegir::nmspace::pack_string(out + out_words, "NIL", 3,
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
        write("  nil: the namespace refused the registration\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    write("  nil: ");
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
            answer_read(port);
            break;
        case aegir::volume::kMethodList:
            answer_list(port);
            break;
        case aegir::volume::kMethodStat:
            answer_stat(port, words, count);
            break;
        case aegir::volume::kMethodOpen:
            answer_open(port, words, count);
            break;
        case aegir::volume::kMethodWrite:
            answer_write(port, words, count);
            break;
        case aegir::volume::kMethodClose:
            answer_close(port);
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
