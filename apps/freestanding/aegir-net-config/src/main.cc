/*
 * netconfig: apply Sys:S/network.manifest (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * It is the configurator the net spec calls `NetConfig`: a command on `C:` run
 * from `Sys:S/Startup-Sequence`. It reads the manifest through the VFS and sets
 * each parameter through the stack's control port -- the same path a live write
 * to `Net:` will take -- so the stack carries no config-file or TOML dependency
 * at all. The manifest names the machine's hostname and, per adapter, DHCP or
 * the static addresses; a malformed one is announced loudly and the adapters it
 * did not reach stay down.
 *
 * Only the *source* of the configuration is new: the stand-in this replaces was
 * already a client of the control port, but it asked every adapter for DHCP
 * because it had no manifest to read.
 */

#include "manifest.h"

#include <aegir/bootstrap.h>
#include <aegir/console_stream.h>
#include <aegir/console_stream_client.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/netcontrol.h>
#include <aegir/vfs.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

constexpr char kManifestPath[] = "Sys:S/network.manifest";
constexpr uint32_t kManifestPathLength = sizeof(kManifestPath) - 1;
/* How much of the manifest one VFS read asks for; the volume clamps it to what
 * one envelope carries (volume::kReadMax). The manifest is streamed, so this is
 * a read size and not a ceiling on the file. */
constexpr uint32_t kReadChunk = 512;

void write_line(char const *label, char const *text) noexcept
{
    aegir::debug_write("      ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

/* The first slot past the ones the bootstrap block named: where a resolved
 * volume capability lands (its own slot, not the receive slot the transfer used
 * first). The same rule the resolver keeps. */
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

/* An adapter's name packed low byte first into a word, up to the stack's eight
 * bytes: the way `describe` answers it, so a manifest name can be compared. */
uint64_t pack_name(char const *text, uint32_t length) noexcept
{
    uint64_t packed = 0;
    uint32_t const count = length < 8 ? length : 8;
    for (uint32_t i = 0; i < count; ++i) {
        packed |= static_cast<uint64_t>(static_cast<uint8_t>(text[i])) << (8 * i);
    }
    return packed;
}

bool describe(aegir::ipc::Consumer const &control, unsigned index, uint64_t *name,
              uint64_t *ipv4, uint64_t *netmask, uint64_t *gateway,
              uint64_t *flags) noexcept
{
    uint64_t const request[1] = {index};
    uint64_t answer[aegir::netcontrol::kDescribeWords] = {0};
    aegir::ipc::WordsReply const reply = control.call_words(
        aegir::netcontrol::kMethodDescribe, request, 1, answer,
        aegir::netcontrol::kDescribeWords);
    if (reply.error != 0 || reply.count < aegir::netcontrol::kDescribeWords) {
        return false;
    }
    *name = answer[0];
    *ipv4 = answer[1];
    *netmask = answer[2];
    *gateway = answer[3];
    *flags = answer[4];
    return true;
}

bool set_word(aegir::ipc::Consumer const &control, unsigned index, uint32_t parameter,
              uint64_t value) noexcept
{
    uint64_t const request[3] = {index, parameter, value};
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply =
        control.call_words(aegir::netcontrol::kMethodSet, request, 3, answer, 1);
    return reply.error == 0 && reply.count >= 1 && answer[0] == 1;
}

bool set_text(aegir::ipc::Consumer const &control, unsigned index, uint32_t parameter,
              char const *text, uint32_t length) noexcept
{
    uint64_t request[3 + (netconfig::kHostnameMax + 7) / 8] = {0};
    request[0] = index;
    request[1] = parameter;
    request[2] = length;
    for (uint32_t i = 0; i < length; ++i) {
        request[3 + i / 8] |=
            static_cast<uint64_t>(static_cast<uint8_t>(text[i])) << (8 * (i % 8));
    }
    uint64_t answer[1] = {0};
    aegir::ipc::WordsReply const reply = control.call_words(
        aegir::netcontrol::kMethodSetText, request, 3 + (length + 7) / 8, answer, 1);
    return reply.error == 0 && reply.count >= 1 && answer[0] == 1;
}

/* What the handlers need: the control port, how many adapters the stack serves,
 * and the tallies for the report. */
struct State {
    aegir::ipc::Consumer control;
    unsigned count;
    unsigned hostname_set;
    unsigned configured;
};

/* The hostname: set it on every adapter, so DHCP option 12 carries the same
 * name everywhere. Applied as the manifest's `hostname` line is read, before
 * any adapter's `dhcp`, because the name must be on the netif when `dhcp_start`
 * runs. */
void on_hostname(void *context, char const *name, uint32_t length) noexcept
{
    auto *const state = static_cast<State *>(context);
    for (unsigned index = 0; index < state->count; ++index) {
        if (set_text(state->control, index, aegir::netcontrol::kParamHostname, name,
                     length)) {
            ++state->hostname_set;
        }
    }
}

/* One adapter's section: find the adapter the stack serves by that name and set
 * its parameters -- DHCP, or the static addresses and then up. */
void on_adapter(void *context, netconfig::Adapter const &adapter) noexcept
{
    auto *const state = static_cast<State *>(context);
    uint64_t const wanted = pack_name(adapter.name, adapter.name_length);
    for (unsigned index = 0; index < state->count; ++index) {
        uint64_t name = 0;
        uint64_t ipv4 = 0;
        uint64_t netmask = 0;
        uint64_t gateway = 0;
        uint64_t flags = 0;
        if (!describe(state->control, index, &name, &ipv4, &netmask, &gateway, &flags)) {
            continue;
        }
        if (name != wanted) {
            continue;
        }
        if (adapter.has_address) {
            (void)set_word(state->control, index, aegir::netcontrol::kParamIpv4Address,
                           adapter.address);
        }
        if (adapter.has_netmask) {
            (void)set_word(state->control, index, aegir::netcontrol::kParamIpv4Netmask,
                           adapter.netmask);
        }
        if (adapter.has_gateway) {
            (void)set_word(state->control, index, aegir::netcontrol::kParamIpv4Gateway,
                           adapter.gateway);
        }
        if (adapter.dhcp) {
            (void)set_word(state->control, index, aegir::netcontrol::kParamDhcp, 1);
        } else if (adapter.has_address) {
            (void)set_word(state->control, index, aegir::netcontrol::kParamUp, 1);
        }
        ++state->configured;
        return;
    }
    /* A section for an adapter the stack does not serve: say so, loudly, and go
     * on -- the other adapters are still configured (specs/net.md's loud
     * failure). */
    aegir::debug_write("      netconfig: no adapter named ");
    aegir::debug_write(adapter.name, adapter.name_length);
    aegir::debug_write("; its section is ignored\n");
}

/* Read Sys:S/network.manifest and feed it to `manifest`, streaming so no buffer
 * holds the file whole. The volume capability was this call's own; drop it so
 * the next resolve into that slot is not refused. */
bool read_manifest(aegir::vfs::Namespace &space, netconfig::Manifest &manifest) noexcept
{
    uint32_t const slot = first_free_slot();
    if (slot == 0 || slot == aegir::bootstrap::kSlotReceiveCap) {
        return false;
    }
    aegir::vfs::Namespace::Resolved resolved{};
    if (!space.resolve(kManifestPath, kManifestPathLength,
                       static_cast<seL4_CPtr>(slot), resolved)) {
        return false;
    }
    aegir::vfs::Volume volume(resolved.volume);
    uint64_t offset = 0;
    bool ok = true;
    for (;;) {
        aegir::vfs::Volume::Bytes bytes{};
        if (!volume.read(resolved.rest, resolved.rest_length, offset, kReadChunk, bytes)) {
            ok = false;
            break;
        }
        if (bytes.count > 0 &&
            !manifest.feed(bytes.data, static_cast<uint32_t>(bytes.count))) {
            ok = false;
            break;
        }
        offset += bytes.count;
        if (bytes.eof || bytes.count == 0) {
            break;
        }
    }
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, static_cast<seL4_CPtr>(slot),
                      aegir::bootstrap::kCNodeBits);
    return ok && manifest.finish();
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

    aegir::ipc::Consumer const control = aegir::ipc::Consumer::find(
        aegir::netcontrol::kPortName, aegir::netcontrol::kPortNameLength);
    /* The console stream every command is handed: a launched command reports its
     * exit through it, or the shell waits forever for it (aegir-echo's shape). */
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

    if (!control.valid()) {
        write_line("FAIL", "netconfig: no control port was given to me");
        finish(1);
    }
    aegir::ipc::Consumer const namespace_port = aegir::vfs::find_namespace();
    if (!namespace_port.valid()) {
        write_line("FAIL", "netconfig: no vfs.namespace was given to me");
        finish(1);
    }
    aegir::vfs::Namespace space(namespace_port);

    aegir::ipc::Reply const listed = control.call(aegir::netcontrol::kMethodList, 0);
    if (listed.error != 0) {
        write_line("FAIL", "netconfig: the stack would not list its adapters");
        finish(1);
    }
    State state{control, static_cast<unsigned>(listed.word), 0, 0};

    netconfig::Manifest manifest;
    netconfig::Manifest::Handler const handler{&state, on_hostname, on_adapter};
    manifest.set_handler(handler);

    if (!read_manifest(space, manifest)) {
        if (manifest.failed()) {
            netconfig::Problem const problem = manifest.problem();
            aegir::debug_write("      netconfig: network.manifest: line ");
            aegir::debug_write_unsigned(problem.line);
            aegir::debug_write(": ");
            aegir::debug_write(problem.message);
            aegir::debug_write("\n");
        } else {
            write_line("FAIL", "netconfig: Sys:S/network.manifest is not there");
        }
        write_line("FAIL", "netconfig: the manifest was not applied; the adapters stay down");
        finish(1);
    }

    aegir::debug_write("      netconfig: ");
    aegir::debug_write_unsigned(state.configured);
    aegir::debug_write(" of ");
    aegir::debug_write_unsigned(state.count);
    aegir::debug_write(" adapters configured, hostname on ");
    aegir::debug_write_unsigned(state.hostname_set);
    aegir::debug_write("\n");

    write_line("netconfig", "ready");
    finish(0);
}
