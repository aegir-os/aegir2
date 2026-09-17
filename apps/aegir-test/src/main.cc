/*
 * aegir-test: the accumulating test bed.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Where a behaviour worth keeping gets a check that keeps it: hello proves
 * the cheapest spawn, this service asks the questions the rest of the
 * system answers, and says out loud whether the answers were right --
 * before it reports ready, because what prints after the boot marker is
 * lost. Today: the VFS's half of the namespace (specs/vfs.md). Resolve a
 * path, and the answer is a capability minted with this service's own
 * badge; read the file the disk was made with through it, and the contents
 * are the checksum -- a reader that walked the wrong sectors does not
 * produce them (scripts/make_disk.py). The write side gets the same
 * treatment on SCRATCH:, the partition that exists for it: create, write
 * across a cluster boundary, read back the recomputed pattern, truncate,
 * and the refusals.
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

aegir::ipc::Consumer g_nmspace;

void write(char const *text)
{
    aegir::debug_write(text);
}

void write(char const *text, uint32_t length)
{
    aegir::debug_write(text, length);
}

/* What the disk was made with: a read that walked the wrong sectors does
 * not produce these. */
constexpr char kAegirTxt[] = "aegir read this file off a disk it enumerated itself\n";
constexpr char kSecondTxt[] = "a second volume, a second service, the same reader\n";

uint32_t text_length(char const *text)
{
    uint32_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

bool same_bytes(char const *a, char const *b, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

/* Resolve `Volume:rest`, trying again until the volume exists: the volumes
 * join the namespace while the boot set is still coming up, and a service
 * that asks too early is told no -- not yet is the same answer as never,
 * so the asking repeats. The capability the answer carries lands in `slot`,
 * minted with this service's own badge, and the volume-relative rest comes
 * back as a string -- an alias composes one the path never contained
 * (specs/vfs.md's Aliases). One static buffer, because the checks resolve
 * one path and use it before resolving the next. */
seL4_CPtr resolve(char const *path, uint32_t path_length, char const **rest,
                  uint32_t *rest_length, seL4_CPtr slot) noexcept
{
    static char rest_buffer[aegir::nmspace::kPathMax];
    for (;;) {
        uint64_t out[aegir::nmspace::kPathMax / 8 + 1];
        uint32_t const out_words =
            aegir::nmspace::pack_string(out, path, path_length, aegir::nmspace::kPathMax);
        uint64_t in[aegir::nmspace::kResolveWords];
        bool cap_arrived = false;
        aegir::ipc::WordsReply const answer =
            g_nmspace.call_transfer(aegir::nmspace::kMethodResolve, out, out_words, 0, in,
                                    aegir::nmspace::kResolveWords, &cap_arrived);
        char const *text = nullptr;
        uint32_t length = 0;
        if (answer.error == 0 && cap_arrived &&
            aegir::nmspace::unpack_string(in, answer.count, aegir::nmspace::kPathMax,
                                          &text, &length) &&
            aegir::ipc::take_received_cap(slot)) {
            for (uint32_t i = 0; i < length; ++i) {
                rest_buffer[i] = text[i];
            }
            *rest = rest_buffer;
            *rest_length = length;
            return slot;
        }
        seL4_Yield();
    }
}

/* Read the whole file, one envelope at a time, and check every byte against
 * what the disk was made with. */
bool read_and_check(seL4_CPtr port, char const *path, uint32_t path_length,
                    char const *expected, uint32_t expected_length) noexcept
{
    aegir::ipc::Consumer volume(port);
    uint64_t offset = 0;
    uint64_t seen = 0;
    bool right = true;
    for (;;) {
        uint64_t out[aegir::nmspace::kPathMax / 8 + 3];
        uint32_t out_words =
            aegir::nmspace::pack_string(out, path, path_length, aegir::nmspace::kPathMax);
        out[out_words++] = offset;
        out[out_words++] = aegir::volume::kReadMax;
        uint64_t in[aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8];
        aegir::ipc::WordsReply const answer = volume.call_words(
            aegir::volume::kMethodRead, out, out_words, in,
            aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8);
        if (answer.error != 0 || answer.count < aegir::volume::kReadHeaderWords) {
            write("  test: a read was refused\n");
            return false;
        }
        uint64_t const count = in[0];
        uint64_t const eof = in[1];
        if (count > aegir::volume::kReadMax ||
            answer.count < aegir::volume::kReadHeaderWords + (count + 7) / 8) {
            write("  test: a read's count does not fit its answer\n");
            return false;
        }
        char const *bytes = reinterpret_cast<char const *>(in + aegir::volume::kReadHeaderWords);
        for (uint64_t i = 0; i < count; ++i) {
            if (seen + i >= expected_length || bytes[i] != expected[seen + i]) {
                right = false;
            }
        }
        seen += count;
        offset += count;
        if (eof != 0 || count == 0) {
            break;
        }
    }
    return right && seen == expected_length;
}

/* The handle side (specs/vfs.md): open with its mode flags, write at the
 * cursor, close. Zero is never a handle, and ~0 is a write whose answer did
 * not come back -- both are the refusal a check reports. */
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
        return ~0ULL;
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

/* mkdir: the path is the whole ask -- the directory it names and every
 * missing component on the way. 1 made-or-existed, 0 refused. */
uint64_t vol_mkdir(seL4_CPtr port, char const *path, uint32_t path_length) noexcept
{
    aegir::ipc::Consumer volume(port);
    uint64_t out[aegir::nmspace::kPathMax / 8 + 1];
    uint32_t const out_words =
        aegir::nmspace::pack_string(out, path, path_length, aegir::nmspace::kPathMax);
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        volume.call_words(aegir::volume::kMethodMkdir, out, out_words, in, 1);
    if (answer.error != 0 || answer.count != 1) {
        return 0;
    }
    return in[0];
}

/* remove: 1 removed, 0 refused -- not found, not empty, open, read-only,
 * or the root. */
uint64_t vol_remove(seL4_CPtr port, char const *path, uint32_t path_length) noexcept
{
    aegir::ipc::Consumer volume(port);
    uint64_t out[aegir::nmspace::kPathMax / 8 + 1];
    uint32_t const out_words =
        aegir::nmspace::pack_string(out, path, path_length, aegir::nmspace::kPathMax);
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        volume.call_words(aegir::volume::kMethodRemove, out, out_words, in, 1);
    if (answer.error != 0 || answer.count != 1) {
        return 0;
    }
    return in[0];
}

/* One read, asking for refusal: true when the volume says no. */
bool read_refused(seL4_CPtr port, char const *path, uint32_t path_length) noexcept
{
    aegir::ipc::Consumer volume(port);
    uint64_t out[aegir::nmspace::kPathMax / 8 + 3];
    uint32_t out_words =
        aegir::nmspace::pack_string(out, path, path_length, aegir::nmspace::kPathMax);
    out[out_words++] = 0;
    out[out_words++] = 1;
    uint64_t in[aegir::volume::kReadHeaderWords + 1];
    aegir::ipc::WordsReply const answer = volume.call_words(
        aegir::volume::kMethodRead, out, out_words, in, aegir::volume::kReadHeaderWords + 1);
    return answer.error != 0 || answer.count < aegir::volume::kReadHeaderWords;
}

/* What the write test writes: a pattern the reader can recompute, so a byte
 * that landed in the wrong place is a byte that reads back wrong. */
uint8_t pattern_at(uint64_t i) noexcept
{
    return static_cast<uint8_t>('a' + (i % 26));
}

/* Read the whole file and check every byte against the pattern. */
bool read_and_check_pattern(seL4_CPtr port, char const *path, uint32_t path_length,
                            uint64_t total) noexcept
{
    aegir::ipc::Consumer volume(port);
    uint64_t offset = 0;
    bool right = true;
    for (;;) {
        uint64_t out[aegir::nmspace::kPathMax / 8 + 3];
        uint32_t out_words =
            aegir::nmspace::pack_string(out, path, path_length, aegir::nmspace::kPathMax);
        out[out_words++] = offset;
        out[out_words++] = aegir::volume::kReadMax;
        uint64_t in[aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8];
        aegir::ipc::WordsReply const answer = volume.call_words(
            aegir::volume::kMethodRead, out, out_words, in,
            aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8);
        if (answer.error != 0 || answer.count < aegir::volume::kReadHeaderWords) {
            return false;
        }
        uint64_t const count = in[0];
        uint64_t const eof = in[1];
        if (count > aegir::volume::kReadMax ||
            answer.count < aegir::volume::kReadHeaderWords + (count + 7) / 8) {
            return false;
        }
        auto const *bytes =
            reinterpret_cast<uint8_t const *>(in + aegir::volume::kReadHeaderWords);
        for (uint64_t i = 0; i < count; ++i) {
            if (offset + i >= total || bytes[i] != pattern_at(offset + i)) {
                right = false;
            }
        }
        offset += count;
        if (eof != 0 || count == 0) {
            break;
        }
    }
    return right && offset == total;
}

/* A login ask: both halves of the credential, one word back -- and a word
 * that is not 0 or 1 says the protocol itself broke, which is a different
 * failure than a refused login. */
uint64_t login(aegir::ipc::Consumer const &port, char const *name,
               char const *secret) noexcept
{
    uint64_t out[aegir::ipc::kMaxWords];
    uint32_t words = aegir::nmspace::pack_string(out, name, text_length(name),
                                                 aegir::authdb::kNameBytes);
    if (words == 0) {
        return ~0ULL;
    }
    uint32_t const secret_words = aegir::nmspace::pack_string(
        out + words, secret, text_length(secret), aegir::authdb::kSecretBytes);
    if (secret_words == 0) {
        return ~0ULL;
    }
    words += secret_words;
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        port.call_words(aegir::auth::kMethodLogin, out, words, in, 1);
    if (answer.error != 0 || answer.count != 1) {
        return ~0ULL;
    }
    return in[0];
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

    g_nmspace = aegir::ipc::Consumer::find(aegir::nmspace::kPortName,
                                           aegir::nmspace::kPortNameLength);
    if (!g_nmspace.valid()) {
        write("  test: FAIL no vfs.namespace\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* Slots for the capabilities resolve hands over: past everything the
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

    unsigned failed = 0;

    /* The first volume's file, end to end: resolve, then read through the
     * capability the answer carried. */
    char const *rest = nullptr;
    uint32_t rest_length = 0;
    seL4_CPtr const aegir_volume =
        resolve("AEGIR:AEGIR.TXT", 15, &rest, &rest_length,
                static_cast<seL4_CPtr>(first_free));
    if (!read_and_check(aegir_volume, rest, rest_length, kAegirTxt,
                        text_length(kAegirTxt))) {
        write("  test: FAIL AEGIR:AEGIR.TXT did not read back what the disk holds\n");
        ++failed;
    } else {
        write("  test: AEGIR:AEGIR.TXT reads back what the disk holds\n");
    }

    /* The same file by the system volume's alias: the disk's type GUID said
     * AEGIR is Sys:, and the alias must answer with the same bytes. */
    seL4_CPtr const sys_volume =
        resolve("Sys:AEGIR.TXT", 13, &rest, &rest_length,
                static_cast<seL4_CPtr>(first_free + 4));
    if (!read_and_check(sys_volume, rest, rest_length, kAegirTxt,
                        text_length(kAegirTxt))) {
        write("  test: FAIL Sys:AEGIR.TXT did not read back what AEGIR: holds\n");
        ++failed;
    } else {
        write("  test: Sys:AEGIR.TXT is AEGIR:AEGIR.TXT by another name\n");
    }

    /* Two components deep: the directory walk finds what the disk build
     * planted, and the bytes are the checksum again. */
    static char const kNestedTxt[] = "two components deep, and the walk found it\n";
    static char const kNestedPath[] = "AEGIR:DOCS/NESTED.TXT";
    seL4_CPtr const nested_volume =
        resolve(kNestedPath, sizeof(kNestedPath) - 1, &rest, &rest_length,
                static_cast<seL4_CPtr>(first_free + 5));
    if (!read_and_check(nested_volume, rest, rest_length, kNestedTxt,
                        text_length(kNestedTxt))) {
        write("  test: FAIL AEGIR:DOCS/NESTED.TXT did not read back through the walk\n");
        ++failed;
    } else {
        write("  test: AEGIR:DOCS/NESTED.TXT reads back, two components deep\n");
    }

    /* The same capability lists the volume's root: the empty rest names the
     * one directory version one knows. */
    {
        aegir::ipc::Consumer volume(aegir_volume);
        uint32_t entries = 0;
        bool listed = false;
        for (uint32_t i = 0;; ++i) {
            uint64_t out[2] = {0, i}; /* the empty path, then the index */
            uint64_t in[aegir::ipc::kMaxWords];
            aegir::ipc::WordsReply const answer = volume.call_words(
                aegir::volume::kMethodList, out, 2, in, aegir::ipc::kMaxWords);
            if (answer.error != 0 || answer.count == 0) {
                break;
            }
            ++entries;
            char const *name = nullptr;
            uint32_t name_length = 0;
            if (aegir::nmspace::unpack_string(in, answer.count, aegir::nmspace::kPathMax,
                                              &name, &name_length) &&
                same_bytes(name, "AEGIR.TXT", 9) && name_length == 9) {
                listed = true;
            }
        }
        if (!listed) {
            write("  test: FAIL AEGIR: listed without AEGIR.TXT\n");
            ++failed;
        } else {
            write("  test: AEGIR: lists AEGIR.TXT among ");
            aegir::debug_write_unsigned(entries);
            write(entries == 1 ? " entry\n" : " entries\n");
        }
    }

    /* The second volume: another service, another partition, the same
     * questions. */
    seL4_CPtr const second_volume =
        resolve("SECOND:SECOND.TXT", 17, &rest, &rest_length,
                static_cast<seL4_CPtr>(first_free + 1));
    if (!read_and_check(second_volume, rest, rest_length, kSecondTxt,
                        text_length(kSecondTxt))) {
        write("  test: FAIL SECOND:SECOND.TXT did not read back what the disk holds\n");
        ++failed;
    } else {
        write("  test: SECOND:SECOND.TXT reads back what the disk holds\n");
    }

    /* And the boot image itself, by its volume name. The manifest's first
     * bytes are its own to change, so what this checks is that the ask is
     * answered -- the byte-exact checks are the FAT volumes', above. */
    seL4_CPtr const initrd_volume =
        resolve("Initrd:services.manifest", 24, &rest, &rest_length,
                static_cast<seL4_CPtr>(first_free + 2));
    {
        aegir::ipc::Consumer volume(initrd_volume);
        uint64_t out[aegir::nmspace::kPathMax / 8 + 3];
        uint32_t out_words = aegir::nmspace::pack_string(out, rest, rest_length,
                                                         aegir::nmspace::kPathMax);
        out[out_words++] = 0;
        out[out_words++] = 32;
        uint64_t in[aegir::volume::kReadHeaderWords + 32 / 8];
        aegir::ipc::WordsReply const answer =
            volume.call_words(aegir::volume::kMethodRead, out, out_words, in,
                              aegir::volume::kReadHeaderWords + 32 / 8);
        char const *bytes = reinterpret_cast<char const *>(in + aegir::volume::kReadHeaderWords);
        if (answer.error != 0 || answer.count < aegir::volume::kReadHeaderWords + 1 ||
            in[0] < 8) {
            write("  test: FAIL Initrd:services.manifest did not read\n");
            ++failed;
        } else {
            write("  test: Initrd:services.manifest begins: ");
            write(bytes, 8);
            write("\n");
        }
    }

    /* The write side, on the volume that exists for it: create, write across
     * a cluster boundary in two calls, close, read back byte-exact, list;
     * then truncate shrinks it, and the refusals -- an existing name without
     * create, a read-only volume, a handle that is not one. */
    seL4_CPtr const scratch_volume =
        resolve("SCRATCH:WROTE.TXT", 17, &rest, &rest_length,
                static_cast<seL4_CPtr>(first_free + 3));
    {
        constexpr uint64_t kTotal = 1100; /* two clusters and a piece */
        uint8_t bytes[700];
        for (uint32_t i = 0; i < sizeof(bytes); ++i) {
            bytes[i] = pattern_at(400 + i);
        }
        uint64_t const handle =
            vol_open(scratch_volume, rest, rest_length,
                     aegir::volume::kOpenCreate | aegir::volume::kOpenTruncate);
        bool ok = handle != 0;
        if (ok) {
            uint8_t first[400];
            for (uint32_t i = 0; i < sizeof(first); ++i) {
                first[i] = pattern_at(i);
            }
            ok = vol_write(scratch_volume, handle, first, sizeof(first)) == sizeof(first) &&
                 vol_write(scratch_volume, handle, bytes, sizeof(bytes)) == sizeof(bytes) &&
                 vol_close(scratch_volume, handle) == 1;
        }
        if (!ok) {
            write("  test: FAIL SCRATCH:WROTE.TXT would not be written\n");
            ++failed;
        } else if (!read_and_check_pattern(scratch_volume, rest, rest_length, kTotal)) {
            write("  test: FAIL SCRATCH:WROTE.TXT did not read back what was written\n");
            ++failed;
        } else {
            write("  test: SCRATCH:WROTE.TXT reads back what was written (");
            aegir::debug_write_unsigned(kTotal);
            write(" bytes across a cluster boundary)\n");
        }
    }
    {
        /* Listed, with its size; then truncated small again. */
        aegir::ipc::Consumer volume(scratch_volume);
        bool listed = false;
        for (uint32_t i = 0;; ++i) {
            uint64_t out[2] = {0, i};
            uint64_t in[aegir::ipc::kMaxWords];
            aegir::ipc::WordsReply const answer = volume.call_words(
                aegir::volume::kMethodList, out, 2, in, aegir::ipc::kMaxWords);
            if (answer.error != 0 || answer.count == 0) {
                break;
            }
            char const *name = nullptr;
            uint32_t name_length = 0;
            if (aegir::nmspace::unpack_string(in, answer.count, aegir::nmspace::kPathMax,
                                              &name, &name_length) &&
                name_length == 9 && same_bytes(name, "WROTE.TXT", 9)) {
                listed = true;
            }
        }
        if (!listed) {
            write("  test: FAIL SCRATCH: lists without WROTE.TXT\n");
            ++failed;
        } else {
            write("  test: SCRATCH: lists WROTE.TXT\n");
        }

        constexpr uint64_t kSmall = 10;
        uint8_t small[kSmall];
        for (uint32_t i = 0; i < sizeof(small); ++i) {
            small[i] = pattern_at(i);
        }
        uint64_t const handle =
            vol_open(scratch_volume, rest, rest_length,
                     aegir::volume::kOpenCreate | aegir::volume::kOpenTruncate);
        bool const shrunk = handle != 0 &&
                            vol_write(scratch_volume, handle, small, sizeof(small)) ==
                                sizeof(small) &&
                            vol_close(scratch_volume, handle) == 1 &&
                            read_and_check_pattern(scratch_volume, rest, rest_length, kSmall);
        if (!shrunk) {
            write("  test: FAIL SCRATCH:WROTE.TXT did not truncate and rewrite\n");
            ++failed;
        } else {
            write("  test: SCRATCH:WROTE.TXT truncates and rewrites\n");
        }
    }
    /* The refusals: an existing name without create, a read-only volume, a
     * handle that is not one. */
    if (vol_open(scratch_volume, rest, rest_length, 0) != 0) {
        write("  test: FAIL an existing name opened without create\n");
        ++failed;
    }
    if (vol_open(initrd_volume, "services.manifest", 17, aegir::volume::kOpenCreate) != 0) {
        write("  test: FAIL the read-only volume took an open\n");
        ++failed;
    }
    if (vol_write(scratch_volume, 9999, nullptr, 0) != 0 || /* no such handle */
        vol_close(scratch_volume, 9999) != 0) {
        write("  test: FAIL a handle that is not one was not refused\n");
        ++failed;
    }

    /* Directories are made, not found: the mmd shape builds the whole chain
     * in one call, the same call again finds what the first made, and a
     * file two components down writes and reads back. */
    {
        static char const kNestPath[] = "NEST/DEEP";
        static char const kMadePath[] = "NEST/DEEP/MADE.TXT";
        static char const kMadeContent[] =
            "made by the system, in a directory it made\n";
        constexpr uint32_t kMadeLength = sizeof(kMadeContent) - 1;
        bool const made = vol_mkdir(scratch_volume, kNestPath, sizeof(kNestPath) - 1) == 1;
        bool const again = vol_mkdir(scratch_volume, kNestPath, sizeof(kNestPath) - 1) == 1;
        uint64_t const handle =
            vol_open(scratch_volume, kMadePath, sizeof(kMadePath) - 1,
                     aegir::volume::kOpenCreate | aegir::volume::kOpenTruncate);
        bool const wrote =
            handle != 0 &&
            vol_write(scratch_volume, handle,
                      reinterpret_cast<uint8_t const *>(kMadeContent), kMadeLength) ==
                kMadeLength &&
            vol_close(scratch_volume, handle) == 1;
        if (!made || !again || !wrote ||
            !read_and_check(scratch_volume, kMadePath, sizeof(kMadePath) - 1,
                            kMadeContent, kMadeLength)) {
            write("  test: FAIL SCRATCH:NEST/DEEP/MADE.TXT did not make, write, "
                  "and read back\n");
            ++failed;
        } else {
            write("  test: SCRATCH:NEST/DEEP/MADE.TXT made, written, read back\n");
        }
        /* The middle directory lists what it holds, as a directory. */
        bool listed = false;
        for (uint32_t i = 0;; ++i) {
            aegir::ipc::Consumer volume(scratch_volume);
            uint64_t out[aegir::nmspace::kPathMax / 8 + 2];
            uint32_t out_words = aegir::nmspace::pack_string(out, "NEST", 4,
                                                             aegir::nmspace::kPathMax);
            out[out_words++] = i;
            uint64_t in[aegir::ipc::kMaxWords];
            aegir::ipc::WordsReply const answer = volume.call_words(
                aegir::volume::kMethodList, out, out_words, in, aegir::ipc::kMaxWords);
            if (answer.error != 0 || answer.count == 0) {
                break;
            }
            char const *name = nullptr;
            uint32_t name_length = 0;
            uint32_t const name_words =
                aegir::nmspace::unpack_string(in, answer.count, aegir::nmspace::kPathMax,
                                              &name, &name_length)
                    ? 1 + (name_length + 7) / 8
                    : 0;
            if (name_words != 0 && answer.count >= name_words + 2 &&
                name_length == 4 && same_bytes(name, "DEEP", 4) &&
                in[name_words + 1] == aegir::volume::kKindDir) {
                listed = true;
            }
        }
        if (!listed) {
            write("  test: FAIL SCRATCH:NEST lists without DEEP\n");
            ++failed;
        } else {
            write("  test: SCRATCH:NEST lists DEEP, a directory\n");
        }
        /* And the read-only volume makes nothing. */
        if (vol_mkdir(initrd_volume, "NEST", 4) != 0) {
            write("  test: FAIL the read-only volume took a mkdir\n");
            ++failed;
        }
    }

    /* remove: a tree dies leaf-first. A file goes and its read is refused;
     * a directory with contents is refused until it is empty; a file an
     * open handle names is refused until the handle closes. */
    {
        static char const kGonePath[] = "NEST/DEEP/GONE.TXT";
        static char const kFullDir[] = "NEST/FULL";
        static char const kFullFile[] = "NEST/FULL/F.TXT";
        static char const kHeldPath[] = "NEST/DEEP/HELD.TXT";
        bool ok = true;
        /* A file is made, then unmade, and its name stops resolving to
         * bytes. */
        uint64_t handle =
            vol_open(scratch_volume, kGonePath, sizeof(kGonePath) - 1,
                     aegir::volume::kOpenCreate | aegir::volume::kOpenTruncate);
        uint8_t const gone_byte = 'g';
        ok = handle != 0 &&
             vol_write(scratch_volume, handle, &gone_byte, 1) == 1 &&
             vol_close(scratch_volume, handle) == 1 &&
             vol_remove(scratch_volume, kGonePath, sizeof(kGonePath) - 1) == 1 &&
             read_refused(scratch_volume, kGonePath, sizeof(kGonePath) - 1);
        /* A non-empty directory refuses until it is empty. */
        ok = ok && vol_mkdir(scratch_volume, kFullDir, sizeof(kFullDir) - 1) == 1;
        handle = vol_open(scratch_volume, kFullFile, sizeof(kFullFile) - 1,
                          aegir::volume::kOpenCreate | aegir::volume::kOpenTruncate);
        ok = ok && handle != 0 && vol_close(scratch_volume, handle) == 1 &&
             vol_remove(scratch_volume, kFullDir, sizeof(kFullDir) - 1) == 0 &&
             vol_remove(scratch_volume, kFullFile, sizeof(kFullFile) - 1) == 1 &&
             vol_remove(scratch_volume, kFullDir, sizeof(kFullDir) - 1) == 1;
        /* A file an open handle names is refused until the handle closes. */
        handle = vol_open(scratch_volume, kHeldPath, sizeof(kHeldPath) - 1,
                          aegir::volume::kOpenCreate | aegir::volume::kOpenTruncate);
        ok = ok && handle != 0 &&
             vol_remove(scratch_volume, kHeldPath, sizeof(kHeldPath) - 1) == 0 &&
             vol_close(scratch_volume, handle) == 1 &&
             vol_remove(scratch_volume, kHeldPath, sizeof(kHeldPath) - 1) == 1;
        /* What is not there is not removed, and the read-only volume keeps
         * everything. */
        ok = ok && vol_remove(scratch_volume, "NEST/DEEP/GONE.TXT",
                              sizeof("NEST/DEEP/GONE.TXT") - 1) == 0 &&
             vol_remove(initrd_volume, "services.manifest", 17) == 0;
        if (!ok) {
            write("  test: FAIL remove did not unmake, leaf-first\n");
            ++failed;
        } else {
            write("  test: remove unmakes, leaf-first, and refuses what it must\n");
        }
    }

    /* auth.login: the entry the build packed is the checksum -- accepted
     * with its secret, and refused the same way for a wrong secret and an
     * unknown name, because the port is not an oracle (specs/auth.md). */
    aegir::ipc::Consumer const auth_login =
        aegir::ipc::Consumer::find(aegir::auth::kPortName, aegir::auth::kPortNameLength);
    if (!auth_login.valid()) {
        write("  test: FAIL no auth.login\n");
        ++failed;
    } else {
        if (login(auth_login, "rroland", "aegir") != 1) {
            write("  test: FAIL the packed user was not authenticated\n");
            ++failed;
        } else if (login(auth_login, "rroland", "wrong") != 0) {
            write("  test: FAIL a wrong secret was not refused\n");
            ++failed;
        } else if (login(auth_login, "nobody", "aegir") != 0) {
            write("  test: FAIL an unknown name was not refused\n");
            ++failed;
        } else {
            write("  test: auth.login accepts the packed user, refuses wrong "
                  "secret and unknown name alike\n");
        }
    }

    /* The session the login started wrote its home: the same file reads
     * back here by the system name, under this service's own badge -- two
     * badges, two names, one file (specs/auth.md's Homes). The refused
     * logins above ran after the successful one, and auth serves again only
     * once the session signalled ready, so the write is already done. The
     * content is the session's constant, this service's the other end. */
    static char const kHomePath[] = "Sys:Homes/rroland/WELCOME.TXT";
    static char const kWelcome[] = "a home of one's own, written by the session\n";
    seL4_CPtr const home_volume =
        resolve(kHomePath, sizeof(kHomePath) - 1, &rest, &rest_length,
                static_cast<seL4_CPtr>(first_free + 6));
    if (!read_and_check(home_volume, rest, rest_length, kWelcome,
                        sizeof(kWelcome) - 1)) {
        write("  test: FAIL Sys:Homes/rroland/WELCOME.TXT is not what the "
              "session wrote\n");
        ++failed;
    } else {
        write("  test: Sys:Homes/rroland/WELCOME.TXT is the session's own words\n");
    }

    if (failed == 0) {
        write("  test: every check passed\n");
    } else {
        write("  test: ");
        aegir::debug_write_unsigned(failed);
        write(" checks FAILED\n");
    }
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
