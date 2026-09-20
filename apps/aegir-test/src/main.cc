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
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/entropy.h>
#include <aegir/framebuffer.h>
#include <aegir/input.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/nmspace.h>
#include <aegir/registry.h>
#include <aegir/volume.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

aegir::ipc::Consumer g_nmspace;

/* Static, not local: an Allocator carries the tables of what it handed
 * out, and this service's stack is pages (the console says the same of
 * its own). The window block's slice mapping is what they serve. */
aegir::mem::Allocator g_test_objects(nullptr);
aegir::mem::Scratch g_test_scratch(nullptr);

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

/* The console's event channel: wait for one event of a kind for a window,
 * scanning past the rest -- the motion on the way to a click, a key's
 * release after its press. The Wait on the notification is the kernel's
 * wait, not a spin; the ring is drained between wakeups. */
constexpr uint16_t kKeyB = 48; /* Linux's KEY_*, which virtio-input carries unchanged */
constexpr uint16_t kKeyC = 46;
constexpr uint16_t kKeyD = 32;
constexpr uint16_t kKeyE = 18;
constexpr uint16_t kKeyG = 34;

bool wait_ring(volatile uint64_t *ring, seL4_CPtr events, uint16_t type,
               uint64_t window, uint64_t *event) noexcept
{
    for (;;) {
        uint64_t word = 0;
        uint64_t id = 0;
        if (aegir::console::ring_take(ring, &word, &id)) {
            if (aegir::input::event_type(word) == type && id == window) {
                *event = word;
                return true;
            }
            continue;
        }
        seL4_Wait(events, nullptr);
    }
}

/* One pressed key, translated: the code it arrived with and the character
 * the console's keymap gave it. */
bool wait_ring_key(volatile uint64_t *ring, seL4_CPtr events, uint64_t window,
                   uint16_t code, char translated) noexcept
{
    uint64_t event = 0;
    while (wait_ring(ring, events, aegir::console::kEventKey, window, &event)) {
        if (aegir::input::event_code(event) == code &&
            (aegir::input::event_value(event) & 0xffff) ==
                static_cast<uint8_t>(translated) &&
            (aegir::input::event_value(event) & aegir::console::kKeyPressed) != 0) {
            return true;
        }
    }
    return false;
}

/* The framebuffer port's two questions (aegir/framebuffer.h): info answered
 * with exactly this geometry, stride, format and glass size, and set_mode
 * applied (true) or refused (false, the two-zero answer). */
bool info_is(aegir::ipc::Consumer const &gpu, uint64_t width, uint64_t height,
             uint64_t phys_width_mm, uint64_t phys_height_mm) noexcept
{
    uint64_t in[aegir::framebuffer::kInfoWords];
    aegir::ipc::WordsReply const answer =
        gpu.call_words(aegir::framebuffer::kMethodInfo, nullptr, 0, in,
                       aegir::framebuffer::kInfoWords);
    return answer.error == 0 && answer.count == aegir::framebuffer::kInfoWords &&
           in[0] == width && in[1] == height && in[2] == width * 4 &&
           in[3] == aegir::framebuffer::kFormatB8G8R8X8 && in[4] == phys_width_mm &&
           in[5] == phys_height_mm;
}

bool set_mode(aegir::ipc::Consumer const &gpu, uint64_t width, uint64_t height) noexcept
{
    uint64_t const out[] = {width, height};
    uint64_t in[2];
    aegir::ipc::WordsReply const answer =
        gpu.call_words(aegir::framebuffer::kMethodSetMode, out, 2, in, 2);
    return answer.error == 0 && answer.count == 2 && in[0] == width && in[1] == height;
}

/* The registry walk -- find the bound row by name, open it -- is the lib's
 * now (aegir/registry.h): a session takes the same walk, and one walk is
 * one implementation. */
using aegir::registry::open_bound;

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

    /* FAT16 writes: the other flavor's entries, the fixed root, and a
     * subdirectory's chain -- the whole write side again, on the volume the
     * disk build made FAT16 on purpose. */
    {
        static char const kF16Path[] = "FAT16:F16.TXT";
        seL4_CPtr const f16_volume =
            resolve(kF16Path, sizeof(kF16Path) - 1, &rest, &rest_length,
                    static_cast<seL4_CPtr>(first_free + 7));
        constexpr uint64_t kTotal = 1100; /* clusters are one sector: three */
        uint8_t bytes[700];
        for (uint32_t i = 0; i < sizeof(bytes); ++i) {
            bytes[i] = pattern_at(400 + i);
        }
        uint64_t handle = vol_open(f16_volume, rest, rest_length,
                                   aegir::volume::kOpenCreate |
                                       aegir::volume::kOpenTruncate);
        bool ok = handle != 0;
        if (ok) {
            uint8_t first[400];
            for (uint32_t i = 0; i < sizeof(first); ++i) {
                first[i] = pattern_at(i);
            }
            ok = vol_write(f16_volume, handle, first, sizeof(first)) == sizeof(first) &&
                 vol_write(f16_volume, handle, bytes, sizeof(bytes)) == sizeof(bytes) &&
                 vol_close(f16_volume, handle) == 1;
        }
        /* A subdirectory on FAT16: the fixed root gives the slot, the chain
         * holds the file. mmd first -- open makes the file, not the way. */
        static char const kSubPath[] = "SUB/IN.TXT";
        bool const in_sub = vol_mkdir(f16_volume, "SUB", 3) == 1 && [&] {
            uint8_t few[64];
            for (uint32_t i = 0; i < sizeof(few); ++i) {
                few[i] = pattern_at(i);
            }
            uint64_t const sub_handle =
                vol_open(f16_volume, kSubPath, sizeof(kSubPath) - 1,
                         aegir::volume::kOpenCreate |
                             aegir::volume::kOpenTruncate);
            return sub_handle != 0 &&
                   vol_write(f16_volume, sub_handle, few, sizeof(few)) == sizeof(few) &&
                   vol_close(f16_volume, sub_handle) == 1 &&
                   read_and_check_pattern(f16_volume, kSubPath,
                                          sizeof(kSubPath) - 1, sizeof(few));
        }();
        bool const readback = read_and_check_pattern(f16_volume, rest, rest_length,
                                                     kTotal);
        bool const removed =
            vol_remove(f16_volume, kSubPath, sizeof(kSubPath) - 1) == 1 &&
            vol_remove(f16_volume, "SUB", 3) == 1 &&
            vol_remove(f16_volume, rest, rest_length) == 1;
        if (!ok || !readback || !in_sub || !removed) {
            write("  test: FAIL FAT16: did not write, nest, and unmake\n");
            ++failed;
        } else {
            write("  test: FAT16: writes, nests, and unmakes -- the other flavor\n");
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

    /* The session-reclaim arc (specs/auth.md): the leaked LEAK.TXT handle
     * is already gone -- auth reaped the badge's handles and unbound its
     * aliases itself when the session exited, and the refused logins above
     * are the barrier that says the reclaim has run. The proof is that the
     * file removes with nobody's reap; the file itself persists, because a
     * reap closes handles, it does not remove names. Sys:, everyone's
     * alias, still answers. */
    {
        static char const kLeakPath[] = "Sys:Homes/rroland/LEAK.TXT";
        seL4_CPtr const leak_volume =
            resolve(kLeakPath, sizeof(kLeakPath) - 1, &rest, &rest_length,
                    static_cast<seL4_CPtr>(first_free + 8));
        bool const died = vol_remove(leak_volume, rest, rest_length) == 1;
        bool const sys_lives =
            read_and_check(sys_volume, "AEGIR.TXT", 9, kAegirTxt,
                           text_length(kAegirTxt));
        if (!died || !sys_lives) {
            write("  test: FAIL the reaper did not come: LEAK.TXT held, or Sys: lost\n");
            ++failed;
        } else {
            write("  test: the session's leaked handle was already reaped; Sys: is everyone's\n");
        }
        /* The slot's cap goes back: the login loop below resolves into it
         * again, and a second resolve onto an occupied slot is refused. */
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                          static_cast<seL4_CPtr>(first_free + 8),
                          aegir::bootstrap::kCNodeBits);
    }

    /* The drain is closed (specs/auth.md's Session reclaim): logins past
     * what the spawn delegation could hold unreclaimed each start a session
     * and come back. A session charges 142 KiB -- auth's reclaim line says
     * so -- so sixteen logins ask the 2 MiB delegation for more than twice
     * what it holds; without the revoke and the slots' return the logins
     * stop early. Each login gets its own barrier -- a refused login auth
     * answers only once this session is reclaimed -- and its own
     * fingerprint: the LEAK.TXT this session created through its Home:,
     * leaked open, and left for the reaper. It removes only because this
     * session made it and this session's handle is gone: a session whose
     * Home: bind was refused leaves no file, and a handle still held
     * refuses the remove. */
    {
        unsigned logins = 0;
        unsigned reclaimed = 0;
        static char const kLeakPath[] = "Sys:Homes/rroland/LEAK.TXT";
        while (logins < 16 && login(auth_login, "rroland", "aegir") == 1) {
            ++logins;
            if (login(auth_login, "rroland", "wrong") != 0) {
                break;
            }
            seL4_CPtr const leak_volume =
                resolve(kLeakPath, sizeof(kLeakPath) - 1, &rest, &rest_length,
                        static_cast<seL4_CPtr>(first_free + 8));
            if (vol_remove(leak_volume, rest, rest_length) != 1) {
                break;
            }
            seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                              static_cast<seL4_CPtr>(first_free + 8),
                              aegir::bootstrap::kCNodeBits);
            ++reclaimed;
        }
        /* The last session's own words, after its barrier said the reclaim
         * has run. */
        seL4_CPtr const home_volume =
            resolve(kHomePath, sizeof(kHomePath) - 1, &rest, &rest_length,
                    static_cast<seL4_CPtr>(first_free + 9));
        bool const wrote = read_and_check(home_volume, rest, rest_length, kWelcome,
                                          sizeof(kWelcome) - 1);
        if (logins != 16 || reclaimed != 16 || !wrote) {
            write("  test: FAIL reclaim did not close the loop: ");
            aegir::debug_write_unsigned(logins);
            write(" logins, ");
            aegir::debug_write_unsigned(reclaimed);
            write(" reclaimed, of 16\n");
            ++failed;
        } else {
            write("  test: 16 logins, 16 sessions run and reclaimed -- the drain is closed\n");
        }
    }

    /* The map introduces (specs/services.md): walk the registry to the
     * entropy driver's row, open it, and the port the answer carries must
     * serve reads -- twice, nonzero, and never the same twice. The slot the
     * opened cap lands in is a fresh one: a second cap onto an occupied
     * slot is refused. */
    {
        aegir::ipc::Consumer const registry = aegir::ipc::Consumer::find(
            aegir::registry::kPortName, aegir::registry::kPortNameLength);
        seL4_CPtr const entropy_slot = static_cast<seL4_CPtr>(first_free + 10);
        bool ok = registry.valid() && open_bound(registry, "rng.virtio0", 11, entropy_slot);
        if (ok) {
            aegir::ipc::Consumer const entropy(entropy_slot);
            uint64_t want = 32;
            uint64_t first_read[4];
            uint64_t second_read[4];
            aegir::ipc::WordsReply const r1 = entropy.call_words(
                aegir::entropy::kMethodRead, &want, 1, first_read, 4);
            aegir::ipc::WordsReply const r2 = entropy.call_words(
                aegir::entropy::kMethodRead, &want, 1, second_read, 4);
            /* The device may fill less than was asked for; what it did fill
             * is the count's bytes, and that is what is compared. */
            uint32_t const filled =
                (r1.count < r2.count ? r1.count : r2.count) * 8;
            bool nonzero = false;
            bool differing = false;
            if (r1.error == 0 && r2.error == 0 && filled != 0) {
                auto const *a = reinterpret_cast<uint8_t const *>(first_read);
                auto const *b = reinterpret_cast<uint8_t const *>(second_read);
                for (uint32_t i = 0; i < filled; ++i) {
                    nonzero = nonzero || a[i] != 0 || b[i] != 0;
                    differing = differing || a[i] != b[i];
                }
            }
            ok = nonzero && differing;
        }
        if (!ok) {
            write("  test: FAIL the registry's open did not reach fresh entropy\n");
            ++failed;
        } else {
            write("  test: devmgr.registry's open reaches rng.virtio0 -- entropy, twice, fresh\n");
        }
    }

    /* The console's channel, ahead of every check that paces by key: the
     * devices are the console's own now (specs/console.md), and what any
     * other process sees of them is what console serves -- so this service
     * attaches, listens, and creates its window first, and every key the
     * runner presses from here arrives routed, in the focused window's
     * ring. The window is invisible until its first damage, so the display
     * checks below still read a bare backdrop. */
    aegir::ipc::Consumer const gui = aegir::ipc::Consumer::find(
        aegir::console::kPortName, aegir::console::kPortNameLength);
    seL4_CPtr const events = static_cast<seL4_CPtr>(first_free + 34);
    uint8_t *slice = nullptr;
    volatile uint64_t *ring = nullptr;
    uint64_t first_window = 0;
    bool channel = false;
    {
        uint64_t untyped_slot = 0;
        uint64_t vspace_slot = 0;
        uint64_t window_base = 0;
        uint32_t window_bytes = 0;
        uint64_t untyped_physical = 0;
        uint32_t untyped_bits = 0;
        uint64_t untyped_address = 0;
        static_cast<void>(aegir::bootstrap::untyped(&untyped_physical, &untyped_bits,
                                                    &untyped_address));
        bool ok = gui.valid() &&
                  aegir::bootstrap::capability("untyped", 7, &untyped_slot) &&
                  aegir::bootstrap::capability("vspace", 6, &vspace_slot) &&
                  aegir::bootstrap::window(&window_base, &window_bytes) &&
                  g_test_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot),
                                               untyped_bits, untyped_physical);
        if (ok) {
            /* The slots below +64 are this block's constants' neighbours;
             * the allocator works past them. */
            g_test_objects.adopt_slots(first_free + 64, (1u << 10) - (first_free + 64), 0);
            ok = g_test_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                                      static_cast<uintptr_t>(window_base),
                                      static_cast<uintptr_t>(window_base + window_bytes),
                                      &g_test_objects);
        }
        uint64_t frame_bits = 0;
        uint64_t frames = 0;
        ok = ok && aegir::console::attach(gui, 2ull << 20, &frame_bits, &frames) &&
             frame_bits == seL4_LargePageBits && frames == 1;
        seL4_CPtr const slice_frame = static_cast<seL4_CPtr>(first_free + 33);
        ok = ok && aegir::console::frame(gui, 0, slice_frame) &&
             aegir::console::listen(gui, events);
        slice = static_cast<uint8_t *>(
            ok ? g_test_scratch.map_large(slice_frame) : nullptr);
        ok = ok && slice != nullptr;
        ring = ok ? aegir::console::event_ring(slice, 2ull << 20) : nullptr;
        first_window = ok ? aegir::console::create_window(gui, 64, 64, 400, 300, 0) : 0;
        ok = ok && first_window != 0;
        if (!ok) {
            write("  test: FAIL the console's channel would not open\n");
            ++failed;
        } else {
            /* The runner moves the pointer from the screen's centre to the
             * window's centre -- (-376,-186) of relative motion -- and
             * clicks: focus lands (and does not raise), and the button-down
             * arrives window-local, (200,150) in a 400x300 window. */
            write("  test: the console's channel -- a click, please\n");
            uint64_t event = 0;
            bool const focused =
                wait_ring(ring, events, aegir::console::kEventFocus, first_window,
                          &event) &&
                aegir::input::event_value(event) == 1;
            bool clicked = false;
            while (focused &&
                   wait_ring(ring, events, aegir::console::kEventPointer,
                             first_window, &event)) {
                if (aegir::input::event_code(event) == aegir::input::kBtnLeft &&
                    aegir::input::event_value(event) == (200u | (150u << 16))) {
                    clicked = true;
                    break;
                }
            }
            if (!focused || !clicked) {
                write("  test: FAIL the click did not focus the window\n");
                ++failed;
            } else {
                channel = true;
                write("  test: the click focused the window, button-down at (200,150) local\n");
            }
        }
    }

    /* The displays, discovered the same way: two heads of one registry row.
     * gpu0 is the console's screen now (specs/console.md) -- its backdrop
     * went up over the driver's bands at boot -- so the port protocol's
     * poking happens on gpu1, the parked head: geometry, the glass's size
     * from EDID, a mode applied and one refused. What the *screens* show is
     * read from outside (scripts/run_target.py): the cue lines pace the
     * runner's screendumps, and gpu0's dumps assert the console's backdrop
     * never moved. */
    {
        aegir::ipc::Consumer const registry = aegir::ipc::Consumer::find(
            aegir::registry::kPortName, aegir::registry::kPortNameLength);
        seL4_CPtr const gpu0_slot = static_cast<seL4_CPtr>(first_free + 12);
        seL4_CPtr const gpu1_slot = static_cast<seL4_CPtr>(first_free + 13);
        bool ok = registry.valid() && open_bound(registry, "gpu.virtio0", 11, gpu0_slot) &&
                  open_bound(registry, "gpu.virtio1", 11, gpu1_slot);
        aegir::ipc::Consumer const gpu0(gpu0_slot);
        aegir::ipc::Consumer const gpu1(gpu1_slot);
        if (ok) {
            ok = info_is(gpu0, 1280, 800, 320, 200) && info_is(gpu1, 1280, 800, 320, 200);
        }
        if (!ok) {
            write("  test: FAIL the two heads did not both answer 1280x800, 320x200 mm\n");
            ++failed;
        } else {
            write("  test: gpu.virtio0 and gpu.virtio1 both answer 1280x800, 320x200 mm\n");
        }
        if (ok) {
            /* This line is the runner's cue to dump both heads at 1280x800
             * -- the drivers' own markers passed long before this service
             * could say it was listening, so the cue is ours -- and the 'b'
             * that says the dumps are done comes back routed, through the
             * focused window's ring. */
            write("  test: both heads answered -- the screens, please\n");
            ok = channel && wait_ring_key(ring, events, first_window, kKeyB, 'b');
        }
        if (ok) {
            /* The parked head shrinks; the console's screen must not move. */
            ok = set_mode(gpu1, 1024, 768) && info_is(gpu0, 1280, 800, 320, 200);
        }
        if (!ok) {
            write("  test: FAIL set_mode 1024x768 was not applied, or the console's head moved\n");
            ++failed;
        } else {
            write("  test: gpu.virtio1 took 1024x768; gpu.virtio0 stands at 1280x800\n");
        }
        if (ok) {
            /* 'c' says the shrunken head's dump is done; then 4K, the window's
             * whole reason for being 32 MiB. */
            ok = channel && wait_ring_key(ring, events, first_window, kKeyC, 'c') &&
                 set_mode(gpu1, 3840, 2160);
        }
        /* A mode the window cannot hold is refused, and the screen keeps what
         * it had: 8192x8192 at 32 bits a pixel is 256 MiB, eight windows. */
        bool const refused = ok && !set_mode(gpu1, 8192, 8192) && info_is(gpu1, 3840, 2160, 320, 200);
        if (!ok) {
            write("  test: FAIL set_mode 3840x2160 was not applied\n");
            ++failed;
        } else if (!refused) {
            write("  test: FAIL a mode past the window was not refused cleanly\n");
            ++failed;
        } else {
            write("  test: gpu.virtio1 took 3840x2160, and 8192x8192 was refused\n");
        }
    }

    /* The gpu's window, asked for and handed over (aegir/registry.h's
     * window and window_frame): the shape the row declared -- window=25 as
     * mega pages, sixteen of them -- and every frame arriving as a
     * capability the receiver may map. Mapping them is the console's first
     * act (specs/console.md); what this block proves is the asking. A frame
     * past the count, and the keyboard's windowless row, are the empty
     * reply. */
    {
        aegir::ipc::Consumer const registry = aegir::ipc::Consumer::find(
            aegir::registry::kPortName, aegir::registry::kPortNameLength);
        int64_t const gpu_row = aegir::registry::find_bound(registry, "gpu.virtio0", 11);
        int64_t const kbd_row = aegir::registry::find_bound(registry, "kbd.virtio0", 11);
        uint64_t page_bits = 0;
        uint64_t pages = 0;
        bool ok = registry.valid() && gpu_row >= 0 && kbd_row >= 0 &&
                  aegir::registry::window_geometry(
                      registry, static_cast<uint64_t>(gpu_row), &page_bits, &pages) &&
                  page_bits == seL4_LargePageBits && pages == 16;
        for (uint64_t f = 0; ok && f < pages; ++f) {
            ok = aegir::registry::window_frame(
                registry, static_cast<uint64_t>(gpu_row), f,
                static_cast<seL4_CPtr>(first_free + 16 + f));
        }
        uint64_t unused_bits = 0;
        uint64_t unused_pages = 0;
        bool const refused =
            ok && !aegir::registry::window_frame(registry, static_cast<uint64_t>(gpu_row),
                                                 pages,
                                                 static_cast<seL4_CPtr>(first_free + 16 + 16)) &&
            !aegir::registry::window_geometry(registry, static_cast<uint64_t>(kbd_row),
                                              &unused_bits, &unused_pages);
        if (!ok || !refused) {
            write("  test: FAIL gpu.virtio0's window did not come over whole, or a refusal answered\n");
            ++failed;
        } else {
            write("  test: gpu.virtio0's window is 16 mega pages, every frame handed over, the rest refused\n");
        }
    }

    /* The window protocol, composited and routed: the white window damaged
     * in over the blue backdrop, a red one overlapping it on top by creation
     * order, the white destroyed and the backdrop redrawn beneath -- and the
     * input that paces it arriving through the ring: the click that refocuses
     * (focus went with the destroyed window), the keymap's 'g', and the
     * pointer's motion in window-local coordinates. What the screen shows is
     * the runner's part -- the cues pace its dumps. */
    if (channel) {
        bool ok = true;
        auto *backing = reinterpret_cast<uint32_t *>(slice);
        for (uint32_t p = 0; p < 400 * 300; ++p) {
            backing[p] = 0x00FFFFFF; /* white */
        }
        ok = aegir::console::damage(gui, first_window, 0, 0, 400, 300);
        write("  test: a window of one's own -- the screen, please\n");
        ok = ok && wait_ring_key(ring, events, first_window, kKeyD, 'd');
        uint64_t const second_window =
            ok ? aegir::console::create_window(gui, 300, 200, 400, 300, 0x80000) : 0;
        ok = ok && second_window != 0;
        if (ok) {
            auto *red = reinterpret_cast<uint32_t *>(slice + 0x80000);
            for (uint32_t p = 0; p < 400 * 300; ++p) {
                red[p] = 0x00FF0000; /* red */
            }
            ok = aegir::console::damage(gui, second_window, 0, 0, 400, 300);
            write("  test: two windows, the newer on top -- the screen, please\n");
            ok = ok && wait_ring_key(ring, events, first_window, kKeyE, 'e');
        }
        if (ok) {
            ok = aegir::console::destroy_window(gui, first_window);
            write("  test: the first window left -- the screen, please\n");
            /* Focus went with the destroyed window, so no key paces this
             * cue: the dump fires on the line, and the next click refocuses. */
        }
        if (ok) {
            /* The pointer stands at the first window's centre; the red one's
             * centre is (+236,+136) away, and the click focuses it. */
            write("  test: the red one takes the focus, please\n");
            uint64_t event = 0;
            bool const focused =
                wait_ring(ring, events, aegir::console::kEventFocus, second_window,
                          &event) &&
                aegir::input::event_value(event) == 1;
            bool clicked = false;
            while (focused &&
                   wait_ring(ring, events, aegir::console::kEventPointer,
                             second_window, &event)) {
                if (aegir::input::event_code(event) == aegir::input::kBtnLeft &&
                    aegir::input::event_value(event) == (200u | (150u << 16))) {
                    clicked = true;
                    break;
                }
            }
            ok = focused && clicked;
        }
        if (ok) {
            /* The keymap is the console's (specs/console.md): what arrives
             * is the raw KEY_G and the translated 'g' in one event. */
            write("  test: a key through the keymap, please\n");
            ok = wait_ring_key(ring, events, second_window, kKeyG, 'g');
        }
        if (ok) {
            /* The tablet's (10000,20000) is the screen's (390,488) -- the
             * axis runs 0..32767 -- and (90,288) in the red window's local
             * coordinates. */
            write("  test: the pointer, window-local, please\n");
            uint64_t event = 0;
            bool moved = false;
            while (wait_ring(ring, events, aegir::console::kEventPointer,
                             second_window, &event)) {
                if (aegir::input::event_code(event) == 0 &&
                    aegir::input::event_value(event) == (90u | (288u << 16))) {
                    moved = true;
                    break;
                }
            }
            ok = moved;
        }
        if (ok) {
            /* The move (specs/window-manager.md): the console repaints the
             * union of the old and new rectangles, and the window reads at
             * its new place. The red one goes to the top-left, clear of the
             * greeter's window and of the login's click. */
            ok = aegir::console::move(gui, second_window, 64, 64);
            write("  test: the red one moved -- the screen, please\n");
        }
        if (ok) {
            /* The resize (specs/window-manager.md): the red one shrinks, and
             * the strip it leaves reads as backdrop. */
            ok = aegir::console::resize(gui, second_window, 200, 150);
            write("  test: the red one resized -- the screen, please\n");
        }
        if (!ok) {
            write("  test: FAIL the console's window protocol did not hold\n");
            ++failed;
        } else {
            write("  test: the console composited two windows, redrew what the destroy uncovered,\n");
            write("        and routed the click, the key, and the pointer\n");
        }
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
