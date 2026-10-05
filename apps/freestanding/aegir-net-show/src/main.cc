/*
 * net: read a Net: parameter through the VFS (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The acceptance's client of the Net: volume: `net <adapter>/<parameter>`
 * resolves `Net:<path>` through the ordinary namespace and reads it with the
 * ordinary file protocol -- the whole point of the live half, that a client
 * needs no new API -- and prints the value it read. It is a command, run from
 * Startup-Sequence, so its line is a clean cue.
 */

#include <aegir/bootstrap.h>
#include <aegir/console_stream.h>
#include <aegir/console_stream_client.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/vfs.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

constexpr char kPrefix[] = "Net:";
constexpr uint32_t kPrefixLength = sizeof(kPrefix) - 1;
constexpr uint32_t kValueMax = 128;

void write_line(char const *label, char const *text) noexcept
{
    aegir::debug_write("      ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

uint32_t text_length(char const *text) noexcept
{
    uint32_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

/* The first slot past the ones the bootstrap block named: where a resolved
 * volume capability lands. The same rule the resolver keeps. */
uint32_t first_free_slot() noexcept
{
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            uint64_t const occupied =
                entry.kind == aegir::bootstrap::EntryKind::Capability ? entry.number
                : entry.kind == aegir::bootstrap::EntryKind::DeviceCapability
                    ? entry.reserved
                    : 0;
            if (occupied != 0 && occupied + 1 > first_free) {
                first_free = occupied + 1;
            }
        }
    }
    return static_cast<uint32_t>(first_free);
}

/* Read `path` whole through the namespace, up to `capacity`. Answers the length,
 * or 0 when the volume refused or there is no namespace. */
uint32_t read_path(char const *path, uint32_t path_length, char *out,
                   uint32_t capacity) noexcept
{
    aegir::vfs::Namespace space = aegir::vfs::Namespace::find();
    if (!space.valid()) {
        return 0;
    }
    seL4_CPtr const slot = first_free_slot();
    if (slot == 0 || slot == aegir::bootstrap::kSlotReceiveCap) {
        return 0;
    }
    aegir::vfs::Namespace::Resolved resolved{};
    if (!space.resolve(path, path_length, slot, resolved)) {
        return 0;
    }
    aegir::vfs::Volume volume(resolved.volume);
    uint32_t total = 0;
    uint64_t offset = 0;
    for (;;) {
        aegir::vfs::Volume::Bytes bytes{};
        if (!volume.read(resolved.rest, resolved.rest_length, offset, capacity - total,
                         bytes)) {
            total = 0;
            break;
        }
        for (uint64_t i = 0; i < bytes.count; ++i) {
            out[total + i] = bytes.data[i];
        }
        total += static_cast<uint32_t>(bytes.count);
        offset += bytes.count;
        if (bytes.eof || bytes.count == 0 || total >= capacity) {
            break;
        }
    }
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, slot, aegir::bootstrap::kCNodeBits);
    return total;
}

/* Write `value` to `path` through the namespace: resolve, open a write handle,
 * write, close. The handle applies the value to the stack when it is closed,
 * so the close is part of the write, not an afterthought. */
bool write_path(char const *path, uint32_t path_length, char const *value,
                uint32_t value_length) noexcept
{
    aegir::vfs::Namespace space = aegir::vfs::Namespace::find();
    if (!space.valid()) {
        return false;
    }
    seL4_CPtr const slot = first_free_slot();
    if (slot == 0 || slot == aegir::bootstrap::kSlotReceiveCap) {
        return false;
    }
    aegir::vfs::Namespace::Resolved resolved{};
    if (!space.resolve(path, path_length, slot, resolved)) {
        return false;
    }
    aegir::vfs::Volume volume(resolved.volume);
    uint64_t const handle = volume.open(resolved.rest, resolved.rest_length, 0);
    bool ok = false;
    if (handle != 0) {
        uint64_t written = 0;
        ok = volume.write(handle, value, value_length, &written) &&
             written == value_length;
        (void)volume.close(handle);
    }
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, slot, aegir::bootstrap::kCNodeBits);
    return ok;
}

}  // namespace

int main(int argc, char *argv[])
{
    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    aegir::ipc::Consumer const stream = aegir::ipc::Consumer::find(
        aegir::console::kStreamPortName, aegir::console::kStreamPortNameLength);
    auto finish = [&](uint64_t status) {
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        if (stream.valid()) {
            uint64_t badge = 0;
            (void)aegir::bootstrap::badge(&badge);
            (void)aegir::console::stream_exit(stream, status, badge);
        }
        aegir::halt();
    };

    if (argc < 2 || argv[1] == nullptr || argv[1][0] == '\0') {
        write_line("FAIL", "net: a Net: path is needed");
        finish(1);
    }
    char const *const relative = argv[1];
    uint32_t const relative_length = text_length(relative);

    char path[kPrefixLength + 128];
    for (uint32_t i = 0; i < kPrefixLength; ++i) {
        path[i] = kPrefix[i];
    }
    for (uint32_t i = 0; i < relative_length && i < 127; ++i) {
        path[kPrefixLength + i] = relative[i];
    }
    uint32_t const path_length = kPrefixLength + relative_length;

    /* A second argument writes: `net NE0/hostname aegir-live` sets the
     * parameter, and the read below shows what it became. */
    if (argc >= 3 && argv[2] != nullptr) {
        char const *const new_value = argv[2];
        uint32_t const new_length = text_length(new_value);
        if (!write_path(path, path_length, new_value, new_length)) {
            write_line("FAIL", "net: the Net: path did not write");
            finish(1);
        }
    }

    char value[kValueMax];
    uint32_t const n = read_path(path, path_length, value, sizeof(value));
    if (n == 0) {
        write_line("FAIL", "net: the Net: path did not read");
        finish(1);
    }

    aegir::debug_write("      net: ");
    aegir::debug_write(relative, relative_length);
    aegir::debug_write(" = ");
    aegir::debug_write(value, n);
    aegir::debug_write("\n");
    write_line("net", "ready");
    finish(0);
}
