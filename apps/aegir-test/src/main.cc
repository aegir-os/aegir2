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
 * produce them (scripts/make_disk.py).
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
 * minted with this service's own badge. */
seL4_CPtr resolve(char const *path, uint32_t path_length, char const **rest,
                  uint32_t *rest_length, seL4_CPtr slot) noexcept
{
    for (;;) {
        uint64_t out[aegir::nmspace::kPathMax / 8 + 1];
        uint32_t const out_words =
            aegir::nmspace::pack_string(out, path, path_length, aegir::nmspace::kPathMax);
        uint64_t in[aegir::nmspace::kResolveWords];
        bool cap_arrived = false;
        aegir::ipc::WordsReply const answer =
            g_nmspace.call_transfer(aegir::nmspace::kMethodResolve, out, out_words, 0, in,
                                    aegir::nmspace::kResolveWords, &cap_arrived);
        if (answer.error == 0 && answer.count == aegir::nmspace::kResolveWords &&
            cap_arrived && in[0] <= path_length && aegir::ipc::take_received_cap(slot)) {
            *rest = path + in[0];
            *rest_length = path_length - static_cast<uint32_t>(in[0]);
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
