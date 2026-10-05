/*
 * Resolving a host name: Sys:S/hosts first, then the stack's DNS. See
 * aegir/resolve.h and specs/net.md.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/resolve.h>

#include <aegir/bootstrap.h>
#include <aegir/ipc/port.h>
#include <aegir/net.h>
#include <aegir/vfs.h>

#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::resolve {
namespace {

constexpr char kHostsPath[] = "Sys:S/hosts";
constexpr uint32_t kHostsPathLength = sizeof(kHostsPath) - 1;
/* A line's ceiling: hosts entries are short, and a longer line is malformed and
 * skipped rather than read into an unbounded buffer. */
constexpr uint32_t kLineMax = 256;
/* How much of the file one read asks for; the volume clamps it. */
constexpr uint32_t kReadChunk = 512;
/* How long DNS is given before the lookup gives up; a name the server does not
 * know may never be answered. */
constexpr uint32_t kDnsTimeoutMs = 2000;

/* The first slot past the ones the bootstrap block named: where a resolved
 * volume capability lands (its own slot, not the receive slot the transfer used
 * first). The same rule the timer and the services keep. */
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

/* A name to the stack's DNS, through the socket port (specs/net.md). The name
 * rides the request's words, low byte first. 0 when the stack has no socket
 * port, the name is too long, or the name does not resolve. */
uint32_t dns_lookup(char const *name, uint32_t length) noexcept
{
    aegir::ipc::Consumer const sockets =
        aegir::ipc::Consumer::find(aegir::net::kPortName, aegir::net::kPortNameLength);
    if (!sockets.valid() || length == 0 || length > aegir::net::kMaxNameBytes) {
        return 0;
    }
    /* The name is at word 2: the length, then a timeout for DNS (which may
     * never answer an unknown name), then the bytes. */
    uint64_t request[2 + (aegir::net::kMaxNameBytes + 7) / 8] = {0};
    request[0] = length;
    request[1] = kDnsTimeoutMs;
    for (uint32_t i = 0; i < length; ++i) {
        request[2 + i / 8] |= static_cast<uint64_t>(static_cast<uint8_t>(name[i]))
                              << (8 * (i % 8));
    }
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const resolved = sockets.call_words(
        aegir::net::kMethodResolve, request, 2 + (length + 7) / 8, answer, 1);
    if (resolved.error != 0 || resolved.count < 1) {
        return 0;
    }
    return static_cast<uint32_t>(answer[0]);
}

bool same_name(char const *a, uint32_t a_length, char const *b, uint32_t b_length) noexcept
{
    if (a_length != b_length) {
        return false;
    }
    for (uint32_t i = 0; i < a_length; ++i) {
        char left = a[i];
        char right = b[i];
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<char>(left + 32);
        }
        if (right >= 'A' && right <= 'Z') {
            right = static_cast<char>(right + 32);
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

/* One `address name [aliases...]` line: if a name matches, answer the address.
 * `#` starts a comment, blank lines are nothing, and anything that is not an
 * address first is ignored. */
void consider_line(char const *line, uint32_t length, char const *name,
                   uint32_t name_length, uint32_t *address, bool *found) noexcept
{
    while (length > 0 &&
           (line[length - 1] == '\r' || line[length - 1] == ' ' || line[length - 1] == '\t')) {
        --length;
    }
    uint32_t at = 0;
    while (at < length && (line[at] == ' ' || line[at] == '\t')) {
        ++at;
    }
    if (at >= length || line[at] == '#') {
        return;
    }
    uint32_t const start = at;
    while (at < length && line[at] != ' ' && line[at] != '\t') {
        ++at;
    }
    char token[16];
    uint32_t length_token = at - start;
    if (length_token >= sizeof(token)) {
        length_token = sizeof(token) - 1;
    }
    for (uint32_t i = 0; i < length_token; ++i) {
        token[i] = line[start + i];
    }
    token[length_token] = '\0';
    uint32_t const parsed = parse_ipv4(token);
    if (parsed == 0) {
        return;
    }
    while (at < length) {
        while (at < length && (line[at] == ' ' || line[at] == '\t')) {
            ++at;
        }
        uint32_t const name_start = at;
        while (at < length && line[at] != ' ' && line[at] != '\t') {
            ++at;
        }
        if (at > name_start &&
            same_name(line + name_start, at - name_start, name, name_length)) {
            *address = parsed;
            *found = true;
            return;
        }
    }
}

/* Read Sys:S/hosts and search it for `name`. False when there is no namespace,
 * no such volume or file, or no matching line -- any of which falls through to
 * DNS. Streams the file a line at a time, so a large file needs no buffer. */
bool hosts_lookup(char const *name, uint32_t name_length, uint32_t *address) noexcept
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
    if (!space.resolve(kHostsPath, kHostsPathLength, slot, resolved)) {
        return false;
    }
    aegir::vfs::Volume volume(resolved.volume);
    char line[kLineMax];
    uint32_t line_length = 0;
    bool overlong = false;
    bool found = false;
    uint64_t offset = 0;
    for (;;) {
        aegir::vfs::Volume::Bytes bytes{};
        if (!volume.read(resolved.rest, resolved.rest_length, offset, kReadChunk, bytes)) {
            break;
        }
        for (uint64_t i = 0; i < bytes.count; ++i) {
            char const c = bytes.data[i];
            if (c == '\n') {
                if (!overlong) {
                    consider_line(line, line_length, name, name_length, address, &found);
                }
                line_length = 0;
                overlong = false;
            } else if (line_length < sizeof(line) - 1) {
                line[line_length++] = c;
            } else {
                overlong = true;
            }
        }
        offset += bytes.count;
        if (bytes.eof || bytes.count == 0) {
            break;
        }
    }
    if (!found && !overlong && line_length > 0) {
        consider_line(line, line_length, name, name_length, address, &found);
    }
    /* The volume capability was this call's own; drop it so the next resolve
     * into that slot is not refused. */
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, slot, aegir::bootstrap::kCNodeBits);
    return found;
}

}  // namespace

uint32_t parse_ipv4(char const *text) noexcept
{
    uint32_t value = 0;
    uint32_t part = 0;
    uint32_t parts = 0;
    uint32_t digits = 0;
    for (char const *p = text;; ++p) {
        char const c = *p;
        if (c >= '0' && c <= '9') {
            part = part * 10 + static_cast<uint32_t>(c - '0');
            if (part > 255 || ++digits > 3) {
                return 0;
            }
        } else if (c == '.' || c == '\0') {
            if (digits == 0 || parts > 3) {
                return 0;
            }
            value |= part << (8 * parts);
            ++parts;
            part = 0;
            digits = 0;
            if (c == '\0') {
                break;
            }
        } else {
            return 0;
        }
    }
    return parts == 4 ? value : 0;
}

bool lookup(char const *name, uint32_t length, uint32_t *address) noexcept
{
    if (name == nullptr || address == nullptr || length == 0) {
        return false;
    }
    if (hosts_lookup(name, length, address)) {
        return true;
    }
    uint32_t const found = dns_lookup(name, length);
    if (found != 0) {
        *address = found;
        return true;
    }
    return false;
}

}  // namespace aegir::resolve
