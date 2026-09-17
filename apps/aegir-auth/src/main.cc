/*
 * aegir-auth: the user database and the login port (specs/auth.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The first slice: read the packed user table from Initrd: through the
 * namespace -- the first system consumer of the VFS, because the database
 * is bytes on a volume and everything that reads bytes on a volume goes
 * through the map -- and serve auth.login from it. A name and a secret in,
 * one word out: 1 authenticated, 0 refused, and an unknown name, a wrong
 * secret and a malformed call are the same 0. No sessions, no elevation;
 * the namespace stays open until there is a user badge to check.
 */

#include <aegir/authdb.h>
#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/nmspace.h>
#include <aegir/volume.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

/* The table's backing store: the memory the manifest granted, used from the
 * front. The table grows until that memory is gone -- the bound is a grant,
 * declared in one place, and reaching it is a loud failure, never a quiet
 * overwrite. */
uint8_t *g_arena = nullptr;
uint64_t g_arena_left = 0;

void *arena_take(uint64_t bytes) noexcept
{
    bytes = (bytes + 7) & ~7ULL;
    if (bytes > g_arena_left) {
        return nullptr;
    }
    void *taken = g_arena;
    g_arena += bytes;
    g_arena_left -= bytes;
    return taken;
}

aegir::authdb::Row const *g_rows = nullptr;
uint32_t g_users = 0;

void write(char const *text)
{
    aegir::debug_write(text);
}

/* One field of a row against a string on the wire: equal lengths, equal
 * bytes. The field is NUL-terminated within its width. */
bool field_is(char const *field, uint32_t field_bytes, char const *text,
              uint32_t length) noexcept
{
    uint32_t held = 0;
    while (held < field_bytes && field[held] != '\0') {
        ++held;
    }
    if (held != length) {
        return false;
    }
    for (uint32_t i = 0; i < length; ++i) {
        if (field[i] != text[i]) {
            return false;
        }
    }
    return true;
}

/* The one method: both halves of the credential, or a refusal that says
 * nothing about which half failed. */
void answer_login(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    char const *name = nullptr;
    uint32_t name_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::authdb::kNameBytes, &name,
                                       &name_length)) {
        port.reply(0);
        return;
    }
    uint32_t const name_words = 1 + (name_length + 7) / 8;
    char const *secret = nullptr;
    uint32_t secret_length = 0;
    if (count < name_words ||
        !aegir::nmspace::unpack_string(words + name_words, count - name_words,
                                       aegir::authdb::kSecretBytes, &secret,
                                       &secret_length)) {
        port.reply(0);
        return;
    }
    for (uint32_t u = 0; u < g_users; ++u) {
        if (field_is(g_rows[u].name, aegir::authdb::kNameBytes, name, name_length) &&
            field_is(g_rows[u].secret, aegir::authdb::kSecretBytes, secret,
                     secret_length)) {
            port.reply(1);
            return;
        }
    }
    port.reply(0);
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

    aegir::ipc::Consumer const nmspace =
        aegir::ipc::Consumer::find(aegir::nmspace::kPortName,
                                   aegir::nmspace::kPortNameLength);
    aegir::ipc::Owner port = aegir::ipc::Owner::find(aegir::auth::kPortName,
                                                     aegir::auth::kPortNameLength);
    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    if (!nmspace.valid() || !port.valid() ||
        !aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address)) {
        write("      FAIL auth: no namespace, no port, or no memory\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    static_cast<void>(memory_physical);
    g_arena = reinterpret_cast<uint8_t *>(memory_address);
    g_arena_left = 1ULL << memory_bits;

    /* The slot the database's capability moves into: past everything the
     * bootstrap block names. */
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

    /* Resolve Initrd:users.db, asking again until the volume exists -- the
     * volumes join the namespace while the boot set is still coming up. */
    constexpr char kPath[] = "Initrd:users.db";
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
        } else {
            seL4_Yield();
        }
    }

    /* Read the whole table into the arena, one envelope at a time. The
     * chunks land contiguously, so what the arena holds when the file ends
     * is the table itself. */
    aegir::ipc::Consumer db(volume);
    uint8_t *table = nullptr;
    uint64_t total = 0;
    uint64_t offset = 0;
    bool read_ok = true;
    for (;;) {
        uint64_t out[aegir::nmspace::kPathMax / 8 + 3];
        uint32_t out_words =
            aegir::nmspace::pack_string(out, rest, rest_length, aegir::nmspace::kPathMax);
        out[out_words++] = offset;
        out[out_words++] = aegir::volume::kReadMax;
        uint64_t in[aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8];
        aegir::ipc::WordsReply const answer = db.call_words(
            aegir::volume::kMethodRead, out, out_words, in,
            aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8);
        if (answer.error != 0 || answer.count < aegir::volume::kReadHeaderWords) {
            read_ok = false;
            break;
        }
        uint64_t const count = in[0];
        uint64_t const eof = in[1];
        if (count > aegir::volume::kReadMax ||
            answer.count < aegir::volume::kReadHeaderWords + (count + 7) / 8) {
            read_ok = false;
            break;
        }
        void *chunk = arena_take(count);
        if (chunk == nullptr) {
            read_ok = false;
            break;
        }
        if (table == nullptr) {
            table = static_cast<uint8_t *>(chunk);
        }
        char const *bytes =
            reinterpret_cast<char const *>(in + aegir::volume::kReadHeaderWords);
        for (uint64_t i = 0; i < count; ++i) {
            static_cast<uint8_t *>(chunk)[i] = bytes[i];
        }
        total += count;
        offset += count;
        if (eof != 0 || count == 0) {
            break;
        }
    }

    /* The header says how many rows follow; the file is the checksum of its
     * own length. */
    if (read_ok && table != nullptr && total >= aegir::authdb::kHeaderBytes) {
        auto const *header = reinterpret_cast<uint32_t const *>(table);
        uint64_t const count = header[2];
        if (header[0] != aegir::authdb::kMagic ||
            header[1] != aegir::authdb::kVersion ||
            total != aegir::authdb::kHeaderBytes + count * sizeof(aegir::authdb::Row)) {
            read_ok = false;
        } else {
            g_rows = reinterpret_cast<aegir::authdb::Row const *>(table +
                                                                  aegir::authdb::kHeaderBytes);
            g_users = static_cast<uint32_t>(count);
        }
    } else {
        read_ok = false;
    }
    if (!read_ok) {
        write("      FAIL auth: the user database would not read or is not one\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    write("      auth: ");
    aegir::debug_write_unsigned(g_users);
    write(g_users == 1 ? " user, serving auth.login\n" : " users, serving auth.login\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        uint32_t const method = port.receive_words(words, aegir::ipc::kMaxWords, &count,
                                                   nullptr);
        if (method == aegir::auth::kMethodLogin) {
            answer_login(port, words, count);
        } else {
            /* A method this version does not know is answered by saying
             * nothing (specs/services.md's versioning rule). */
            port.reply_words(nullptr, 0);
        }
    }
}
