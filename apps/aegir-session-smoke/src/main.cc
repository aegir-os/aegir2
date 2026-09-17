/*
 * aegir-session-smoke: the first session, and the proof one happened.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Auth starts one of these when a login succeeds (specs/auth.md): it is a
 * user process, which today means exactly one thing the system services
 * are not -- the badge it carries has the user bit set (bit 62 of the
 * designed badge space, specs/authority.md), and auth minted it. There is
 * no input path yet, so the session is not interactive: what it does is
 * walk the same path any client walks -- log who it is, resolve a name
 * through the namespace, read what the name points at -- and the evidence
 * is the logger's lines, which render the badge of every caller.
 *
 * What it demonstrates:
 *   - auth's spawn kit works: this process was loaded out of the initrd
 *     copy auth was delegated, into an address space from auth's pool,
 *     with its objects retyped from auth's untyped;
 *   - the badge auth minted arrives: the logger prints it, and a number
 *     with bit 62 set is a user where the boot set's badges are small;
 *   - the namespace is open to a user badge, which is the decided shape
 *     until volumes have owners (specs/auth.md).
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/nmspace.h>
#include <aegir/volume.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

void write(char const *text)
{
    aegir::debug_write(text);
}

void write(char const *text, uint32_t length)
{
    aegir::debug_write(text, length);
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    /* The log line is the point: the logger renders the caller's badge, and
     * this caller's badge is the one auth minted for the user who logged
     * in -- bit 62 set, the user row in its middle, the serial at the
     * bottom (specs/authority.md). */
    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    aegir::ipc::Consumer const nmspace =
        aegir::ipc::Consumer::find(aegir::nmspace::kPortName,
                                   aegir::nmspace::kPortNameLength);
    if (!nmspace.valid()) {
        write("  session.smoke: FAIL no vfs.namespace\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The slot the resolved capability lands in: past everything the
     * bootstrap block names, which are ours. */
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            if (entry.kind == aegir::bootstrap::EntryKind::Capability &&
                entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            }
        }
    }

    /* Resolve the boot image's manifest and read its first bytes: the
     * smallest end-to-end ask a client can make -- a name in, a capability
     * back, bytes through it. Asking repeats until the volume exists: the
     * namespace answers "not yet" and "never" the same way, so the asking
     * is the wait (specs/vfs.md). */
    constexpr char kPath[] = "Initrd:services.manifest";
    seL4_CPtr volume = 0;
    char const *rest = nullptr;
    uint32_t rest_length = 0;
    while (volume == 0) {
        uint64_t out[aegir::nmspace::kPathMax / 8 + 1];
        uint32_t const out_words = aegir::nmspace::pack_string(
            out, kPath, sizeof(kPath) - 1, aegir::nmspace::kPathMax);
        uint64_t in[aegir::nmspace::kResolveWords];
        bool cap_arrived = false;
        aegir::ipc::WordsReply const answer = nmspace.call_transfer(
            aegir::nmspace::kMethodResolve, out, out_words, 0, in,
            aegir::nmspace::kResolveWords, &cap_arrived);
        if (answer.error == 0 && answer.count == aegir::nmspace::kResolveWords &&
            cap_arrived && in[0] <= sizeof(kPath) - 1 &&
            aegir::ipc::take_received_cap(static_cast<seL4_CPtr>(first_free))) {
            volume = static_cast<seL4_CPtr>(first_free);
            rest = kPath + in[0];
            rest_length = sizeof(kPath) - 1 - static_cast<uint32_t>(in[0]);
            break;
        }
        seL4_Yield();
    }

    {
        aegir::ipc::Consumer const file(volume);
        uint64_t out[aegir::nmspace::kPathMax / 8 + 3];
        uint32_t out_words =
            aegir::nmspace::pack_string(out, rest, rest_length, aegir::nmspace::kPathMax);
        out[out_words++] = 0;
        out[out_words++] = 32;
        uint64_t in[aegir::volume::kReadHeaderWords + 32 / 8];
        aegir::ipc::WordsReply const answer =
            file.call_words(aegir::volume::kMethodRead, out, out_words, in,
                            aegir::volume::kReadHeaderWords + 32 / 8);
        char const *bytes = reinterpret_cast<char const *>(in + aegir::volume::kReadHeaderWords);
        if (answer.error != 0 || answer.count < aegir::volume::kReadHeaderWords + 1 ||
            in[0] < 8) {
            write("  session.smoke: FAIL Initrd:services.manifest did not read\n");
        } else {
            write("  session.smoke: Initrd:services.manifest begins: ");
            write(bytes, 8);
            write("\n");
        }
    }

    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Ready));
    }
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
