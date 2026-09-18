/*
 * aegir-session-smoke: the first session, and the proof one happened.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Auth starts one of these when a login succeeds (specs/auth.md): it is a
 * user process, which today means exactly one thing the system services
 * are not -- the badge it carries has the user bit set (bit 62 of the
 * designed badge space, specs/authority.md), and auth minted it. What it
 * does is walk the same paths any client walks -- log who it is, resolve a
 * name through the namespace, read what the name points at -- and the
 * evidence is the logger's lines, which render the badge of every caller.
 * Input it has none of: the devices are the console's (specs/console.md),
 * and a session's events arrive as windows' events when the desktop lands.
 *
 * What it demonstrates:
 *   - auth's spawn kit works: this process was loaded out of the initrd
 *     copy auth was delegated, into an address space from auth's pool,
 *     with its objects retyped from auth's untyped;
 *   - the badge auth minted arrives: the logger prints it, and a number
 *     with bit 62 set is a user where the boot set's badges are small;
 *   - the namespace is open to a user badge, which is the decided shape
 *     until volumes have owners (specs/auth.md);
 *   - Home: is this badge's own: auth ensured the directory and bound the
 *     alias, and the session writes into it -- the test service reads the
 *     same file back through Sys:, two badges and two names for one file.
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

/* The handle side of the volume protocol (specs/vfs.md), the smallest
 * shape of it: open with the mode flags, write at the cursor, close.
 * Zero is never a handle. */
uint64_t vol_open(seL4_CPtr port, char const *path, uint32_t path_length,
                  uint64_t flags) noexcept
{
    aegir::ipc::Consumer volume(port);
    uint64_t out[aegir::nmspace::kPathMax / 8 + 2];
    uint32_t const out_words =
        aegir::nmspace::pack_string(out, path, path_length, aegir::nmspace::kPathMax);
    out[out_words] = flags;
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        volume.call_words(aegir::volume::kMethodOpen, out, out_words + 1, in, 1);
    if (answer.error != 0 || answer.count != 1) {
        return 0;
    }
    return in[0];
}

uint64_t vol_write(seL4_CPtr port, uint64_t handle, uint8_t const *bytes,
                   uint32_t count) noexcept
{
    aegir::ipc::Consumer volume(port);
    uint64_t out[2 + aegir::volume::kWriteMax / 8];
    out[0] = handle;
    out[1] = count;
    auto *packed = reinterpret_cast<uint8_t *>(out + 2);
    for (uint32_t i = 0; i < count; ++i) {
        packed[i] = bytes[i];
    }
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        volume.call_words(aegir::volume::kMethodWrite, out, 2 + (count + 7) / 8, in, 1);
    if (answer.error != 0 || answer.count != 1) {
        return 0;
    }
    return in[0];
}

uint64_t vol_close(seL4_CPtr port, uint64_t handle) noexcept
{
    aegir::ipc::Consumer volume(port);
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        volume.call_words(aegir::volume::kMethodClose, &handle, 1, in, 1);
    if (answer.error != 0 || answer.count != 1) {
        return 0;
    }
    return in[0];
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
    static char rest_buffer[aegir::nmspace::kPathMax];
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
        char const *text = nullptr;
        uint32_t length = 0;
        if (answer.error == 0 && cap_arrived &&
            aegir::nmspace::unpack_string(in, answer.count, aegir::nmspace::kPathMax,
                                          &text, &length) &&
            aegir::ipc::take_received_cap(static_cast<seL4_CPtr>(first_free))) {
            volume = static_cast<seL4_CPtr>(first_free);
            for (uint32_t i = 0; i < length; ++i) {
                rest_buffer[i] = text[i];
            }
            rest = rest_buffer;
            rest_length = length;
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

    /* Home: is this badge's own (specs/auth.md's Homes): auth ensured the
     * directory and bound the alias before this process started, so the
     * resolve is the whole ask. Create, write, close -- and the
     * test service reads the same bytes through Sys:Homes/<user>/ under
     * its own badge, which is what makes this a fact about the namespace
     * and not about the session's say-so. The content is the checksum, and
     * aegir-test's copy of it is the other end. */
    static char const kWelcomePath[] = "Home:WELCOME.TXT";
    static char const kWelcome[] = "a home of one's own, written by the session\n";
    {
        seL4_CPtr home_volume = 0;
        uint64_t out[aegir::nmspace::kPathMax / 8 + 1];
        uint32_t const out_words = aegir::nmspace::pack_string(
            out, kWelcomePath, sizeof(kWelcomePath) - 1, aegir::nmspace::kPathMax);
        uint64_t in[aegir::nmspace::kResolveWords];
        bool cap_arrived = false;
        aegir::ipc::WordsReply const answer = nmspace.call_transfer(
            aegir::nmspace::kMethodResolve, out, out_words, 0, in,
            aegir::nmspace::kResolveWords, &cap_arrived);
        char const *text = nullptr;
        uint32_t length = 0;
        if (answer.error == 0 && cap_arrived &&
            aegir::nmspace::unpack_string(in, answer.count, aegir::nmspace::kPathMax,
                                          &text, &length) &&
            aegir::ipc::take_received_cap(static_cast<seL4_CPtr>(first_free + 1))) {
            home_volume = static_cast<seL4_CPtr>(first_free + 1);
        }
        bool wrote = false;
        if (home_volume != 0) {
            /* The rest the answer named: the volume-relative path the alias
             * composed, which open walks like any other. */
            uint64_t const handle =
                vol_open(home_volume, text, length,
                         aegir::volume::kOpenCreate | aegir::volume::kOpenTruncate);
            wrote = handle != 0 &&
                    vol_write(home_volume, handle,
                              reinterpret_cast<uint8_t const *>(kWelcome),
                              sizeof(kWelcome) - 1) == sizeof(kWelcome) - 1 &&
                    vol_close(home_volume, handle) == 1;
        }
        if (!wrote) {
            write("  session.smoke: FAIL Home:WELCOME.TXT would not be written\n");
        } else {
            write("  session.smoke: Home:WELCOME.TXT written -- ");
            write(kWelcome, sizeof(kWelcome) - 1);
        }
        /* One handle left open on purpose: a session that halts without
         * closing is what the volume protocol's reap exists for, and the
         * test service drops it (specs/vfs.md). The file sits beside
         * WELCOME.TXT -- the resolved rest's directory is this home, and
         * the test removes the file once the handle is gone. */
        if (home_volume != 0 && length > sizeof("WELCOME.TXT") - 1) {
            char leak[aegir::nmspace::kPathMax];
            uint32_t const prefix = length - (sizeof("WELCOME.TXT") - 1);
            for (uint32_t i = 0; i < prefix; ++i) {
                leak[i] = text[i];
            }
            for (uint32_t i = 0; i < sizeof("LEAK.TXT") - 1; ++i) {
                leak[prefix + i] = "LEAK.TXT"[i];
            }
            uint32_t const leak_length = prefix + sizeof("LEAK.TXT") - 1;
            uint64_t const leaked =
                vol_open(home_volume, leak, leak_length,
                         aegir::volume::kOpenCreate | aegir::volume::kOpenTruncate);
            write(leaked != 0 ? "  session.smoke: LEAK.TXT left open for the reaper\n"
                              : "  session.smoke: FAIL LEAK.TXT would not open\n");
        }
    }

    /* No input path of its own: the devices are the console's, exclusively
     * (specs/console.md), and a session's events arrive as windows' events
     * when the desktop arc lands. */

    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Ready));
    }
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
