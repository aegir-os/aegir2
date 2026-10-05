/*
 * aegir-net-volume: the Net: filesystem view (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The live half of the network: `Net:<adapter>/<parameter>` is a synthetic
 * volume that reads and reprograms the running stack. One directory per
 * adapter, one file per parameter, the parameter's value as text. It is a
 * volume like any other -- it registers with the VFS and answers the volume
 * protocol (aegir/volume.h) -- so a client reads and writes it with the
 * ordinary file protocol and needs no new API.
 *
 * The service is the volume; the stack is untouched. Every value it answers,
 * and every value it applies, goes through the stack's control port
 * (aegir/netcontrol.h) -- the same port the boot manifest's reader uses -- so
 * the stack carries no filesystem dependency and boot config and live config
 * are one act seen at two times. The adapter names come from the stack
 * (`describe`), which names itself the way a block device does.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/netcontrol.h>
#include <aegir/nmspace.h>
#include <aegir/volume.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

aegir::ipc::Consumer g_control{};

void write(char const *text) noexcept
{
    aegir::debug_write(text);
}

void write(char const *text, uint32_t length) noexcept
{
    aegir::debug_write(text, length);
}

/* One adapter, as the stack describes it. */
struct Adapter {
    char name[16];
    uint32_t name_length;
    uint32_t ipv4;
    uint32_t netmask;
    uint32_t gateway;
    uint64_t flags;
};

bool describe(unsigned index, Adapter *adapter) noexcept
{
    uint64_t const request[1] = {index};
    uint64_t answer[aegir::netcontrol::kDescribeWords] = {0};
    aegir::ipc::WordsReply const reply = g_control.call_words(
        aegir::netcontrol::kMethodDescribe, request, 1, answer,
        aegir::netcontrol::kDescribeWords);
    if (reply.error != 0 || reply.count < aegir::netcontrol::kDescribeWords) {
        return false;
    }
    uint64_t const packed = answer[0];
    uint32_t length = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        char const c = static_cast<char>((packed >> (8 * i)) & 0xff);
        if (c == '\0') {
            break;
        }
        adapter->name[length++] = c;
    }
    adapter->name[length] = '\0';
    adapter->name_length = length;
    adapter->ipv4 = static_cast<uint32_t>(answer[1]);
    adapter->netmask = static_cast<uint32_t>(answer[2]);
    adapter->gateway = static_cast<uint32_t>(answer[3]);
    adapter->flags = answer[4];
    return true;
}

/* How many adapters the stack serves. */
unsigned adapter_count() noexcept
{
    aegir::ipc::Reply const reply =
        g_control.call(aegir::netcontrol::kMethodList, 0);
    return reply.error == 0 ? static_cast<unsigned>(reply.word) : 0;
}

/* The adapter named `name`, by index, or false. */
bool find_adapter(char const *name, uint32_t length, unsigned *index_out,
                  Adapter *adapter_out) noexcept
{
    unsigned const count = adapter_count();
    for (unsigned index = 0; index < count; ++index) {
        Adapter adapter{};
        if (!describe(index, &adapter)) {
            continue;
        }
        if (adapter.name_length == length) {
            bool same = true;
            for (uint32_t i = 0; i < length; ++i) {
                if (adapter.name[i] != name[i]) {
                    same = false;
                    break;
                }
            }
            if (same) {
                *index_out = index;
                *adapter_out = adapter;
                return true;
            }
        }
    }
    return false;
}

/* The parameter files, in the order a listing answers them. The ones the
 * control port cannot yet answer -- the MAC, the MTU, DNS, statistics -- land
 * when it carries them. */
enum Parameter {
    kAddress,
    kNetmask,
    kGateway,
    kDhcp,
    kLink,
    kState,
    kHostname,
    kParameterCount,
};

constexpr char const *kNames[kParameterCount] = {
    "ipv4_address", "ipv4_netmask", "ipv4_gateway",
    "dhcp",         "link",          "state",
    "hostname",
};

uint32_t text_length(char const *text) noexcept
{
    uint32_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

void put_text(char *out, uint32_t *length, char const *text) noexcept
{
    uint32_t const n = text_length(text);
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = text[i];
    }
    *length = n;
}

void put_address(char *out, uint32_t *length, uint32_t address) noexcept
{
    uint32_t at = 0;
    for (uint32_t octet = 0; octet < 4; ++octet) {
        if (octet != 0) {
            out[at++] = '.';
        }
        uint32_t value = (address >> (8 * octet)) & 0xff;
        char digits[3];
        uint32_t count = 0;
        if (value == 0) {
            digits[count++] = '0';
        }
        while (value != 0 && count < 3) {
            digits[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
        while (count > 0) {
            out[at++] = digits[--count];
        }
    }
    *length = at;
}

/* One parameter's value as text, into `out` (which the caller sizes past the
 * widest: a hostname, 63 bytes). False when the parameter is one the control
 * port cannot answer -- the file is then empty. */
bool read_parameter(unsigned index, Adapter const &adapter, unsigned parameter,
                    char *out, uint32_t capacity, uint32_t *length) noexcept
{
    switch (parameter) {
    case kAddress:
        put_address(out, length, adapter.ipv4);
        return true;
    case kNetmask:
        put_address(out, length, adapter.netmask);
        return true;
    case kGateway:
        put_address(out, length, adapter.gateway);
        return true;
    case kDhcp:
        out[0] = (adapter.flags & aegir::netcontrol::kStateDhcp) != 0 ? '1' : '0';
        *length = 1;
        return true;
    case kLink:
        put_text(out, length,
                 (adapter.flags & aegir::netcontrol::kStateLinkUp) != 0 ? "up" : "down");
        return true;
    case kState:
        put_text(out, length,
                 (adapter.flags & aegir::netcontrol::kStateUp) != 0 ? "up" : "down");
        return true;
    case kHostname: {
        uint64_t const request[2] = {index, aegir::netcontrol::kParamHostname};
        uint64_t answer[1 + aegir::ipc::kMaxWords] = {0};
        aegir::ipc::WordsReply const reply = g_control.call_words(
            aegir::netcontrol::kMethodGetText, request, 2, answer,
            1 + aegir::ipc::kMaxWords);
        if (reply.error != 0 || reply.count < 1) {
            return false;
        }
        uint32_t const n = static_cast<uint32_t>(answer[0]);
        if (n == 0 || n > capacity) {
            return false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            out[i] = static_cast<char>((answer[1 + i / 8] >> (8 * (i % 8))) & 0xff);
        }
        *length = n;
        return true;
    }
    default:
        return false;
    }
}

/* Split "adapter/parameter" into its two parts. False when there is no '/'. */
bool split(char const *path, uint32_t length, char const *&adapter, uint32_t &adapter_length,
           char const *&parameter, uint32_t &parameter_length) noexcept
{
    for (uint32_t i = 0; i < length; ++i) {
        if (path[i] == '/') {
            adapter = path;
            adapter_length = i;
            parameter = path + i + 1;
            parameter_length = length - i - 1;
            return adapter_length > 0 && parameter_length > 0;
        }
    }
    return false;
}

bool find_parameter(char const *name, uint32_t length, unsigned *parameter) noexcept
{
    for (unsigned i = 0; i < kParameterCount; ++i) {
        bool same = true;
        uint32_t const n = text_length(kNames[i]);
        if (n != length) {
            same = false;
        }
        for (uint32_t b = 0; same && b < length; ++b) {
            if (kNames[i][b] != name[b]) {
                same = false;
            }
        }
        if (same) {
            *parameter = i;
            return true;
        }
    }
    return false;
}

/* Resolve a path to a parameter's value: false when it is not
 * `<adapter>/<parameter>`, or the adapter is one the stack does not serve. */
bool lookup(char const *path, uint32_t length, char *value, uint32_t capacity,
            uint32_t *value_length) noexcept
{
    char const *adapter_name = nullptr;
    uint32_t adapter_name_length = 0;
    char const *parameter_name = nullptr;
    uint32_t parameter_name_length = 0;
    if (!split(path, length, adapter_name, adapter_name_length, parameter_name,
               parameter_name_length)) {
        return false;
    }
    unsigned index = 0;
    Adapter adapter{};
    if (!find_adapter(adapter_name, adapter_name_length, &index, &adapter)) {
        return false;
    }
    unsigned parameter = 0;
    if (!find_parameter(parameter_name, parameter_name_length, &parameter)) {
        return false;
    }
    return read_parameter(index, adapter, parameter, value, capacity, value_length);
}

/* Unpack a path from a method's words. */
bool path_of(uint64_t const *words, uint32_t count, char const **text, uint32_t *length,
             uint32_t *path_words) noexcept
{
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, text,
                                       length)) {
        return false;
    }
    *path_words = 1 + (*length + 7) / 8;
    return true;
}

void reply_nothing(aegir::ipc::Owner &port) noexcept
{
    port.reply_words(nullptr, 0);
}

/* A list answer: the entry's name as a string, then its size, kind and mtime. */
void reply_entry(aegir::ipc::Owner &port, char const *name, uint32_t name_length,
                 uint64_t size, uint64_t kind) noexcept
{
    uint64_t answer[1 + 16 + aegir::volume::kListTailWords] = {0};
    uint32_t const words = aegir::nmspace::pack_string(answer, name, name_length,
                                                       aegir::nmspace::kNameMax);
    if (words == 0) {
        reply_nothing(port);
        return;
    }
    answer[words] = size;
    answer[words + 1] = kind;
    answer[words + 2] = 0;
    port.reply_words(answer, words + aegir::volume::kListTailWords);
}

/* list: the root lists adapters (directories); an adapter lists its parameter
 * files. The cursor is the index; past the end is an empty reply. */
void answer_list(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    uint32_t path_words = 0;
    if (!path_of(words, count, &path, &path_length, &path_words) ||
        count < path_words + 1) {
        reply_nothing(port);
        return;
    }
    uint64_t const index = words[path_words];

    if (path_length == 0) {
        Adapter adapter{};
        if (index >= adapter_count() ||
            !describe(static_cast<unsigned>(index), &adapter)) {
            reply_nothing(port);
            return;
        }
        reply_entry(port, adapter.name, adapter.name_length, 0, aegir::volume::kKindDir);
        return;
    }
    unsigned adapter_index = 0;
    Adapter adapter{};
    if (!find_adapter(path, path_length, &adapter_index, &adapter)) {
        reply_nothing(port);
        return;
    }
    if (index >= kParameterCount) {
        reply_nothing(port);
        return;
    }
    char value[96];
    uint32_t value_length = 0;
    (void)read_parameter(adapter_index, adapter, static_cast<unsigned>(index), value,
                         sizeof(value), &value_length);
    char const *name = kNames[index];
    reply_entry(port, name, text_length(name), value_length, aegir::volume::kKindFile);
}

/* stat: the root and each adapter are directories; a parameter is a file whose
 * size is its value's length. */
void answer_stat(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    uint32_t path_words = 0;
    if (!path_of(words, count, &path, &path_length, &path_words)) {
        reply_nothing(port);
        return;
    }
    uint64_t kind = aegir::volume::kKindDir;
    uint64_t size = 0;
    if (path_length != 0) {
        unsigned adapter_index = 0;
        Adapter adapter{};
        char const *adapter_name = nullptr;
        uint32_t adapter_name_length = 0;
        char const *parameter_name = nullptr;
        uint32_t parameter_name_length = 0;
        if (split(path, path_length, adapter_name, adapter_name_length, parameter_name,
                  parameter_name_length)) {
            unsigned parameter = 0;
            char value[96];
            uint32_t value_length = 0;
            if (find_adapter(adapter_name, adapter_name_length, &adapter_index, &adapter) &&
                find_parameter(parameter_name, parameter_name_length, &parameter) &&
                read_parameter(adapter_index, adapter, parameter, value, sizeof(value),
                               &value_length)) {
                kind = aegir::volume::kKindFile;
                size = value_length;
            } else {
                reply_nothing(port);
                return;
            }
        } else if (find_adapter(path, path_length, &adapter_index, &adapter)) {
            kind = aegir::volume::kKindDir;
        } else {
            reply_nothing(port);
            return;
        }
    }
    uint64_t answer[aegir::volume::kStatTailWords] = {kind, size, 0};
    port.reply_words(answer, aegir::volume::kStatTailWords);
}

/* read: a parameter's value as text, from an offset. A path that is not a
 * parameter reads as an empty file at its end. */
void answer_read(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    uint32_t path_words = 0;
    if (!path_of(words, count, &path, &path_length, &path_words) ||
        count < path_words + 2) {
        reply_nothing(port);
        return;
    }
    uint64_t const offset = words[path_words];
    uint64_t const maximum = words[path_words + 1];

    char value[96];
    uint32_t value_length = 0;
    if (!lookup(path, path_length, value, sizeof(value), &value_length) ||
        offset >= value_length) {
        uint64_t answer[aegir::volume::kReadHeaderWords] = {0, 1};
        port.reply_words(answer, aegir::volume::kReadHeaderWords);
        return;
    }
    uint32_t n = value_length - static_cast<uint32_t>(offset);
    if (n > maximum) {
        n = static_cast<uint32_t>(maximum);
    }
    uint64_t answer[aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8] = {0};
    answer[0] = n;
    answer[1] = (offset + n >= value_length) ? 1 : 0;
    char *bytes = reinterpret_cast<char *>(answer + aegir::volume::kReadHeaderWords);
    for (uint32_t i = 0; i < n; ++i) {
        bytes[i] = value[offset + i];
    }
    port.reply_words(answer, aegir::volume::kReadHeaderWords + (n + 7) / 8);
}

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

    g_control = aegir::ipc::Consumer::find(aegir::netcontrol::kPortName,
                                           aegir::netcontrol::kPortNameLength);
    aegir::ipc::Owner port = aegir::ipc::Owner::find("vol.net", 7);
    aegir::ipc::Consumer const nmspace =
        aegir::ipc::Consumer::find(aegir::nmspace::kPortName, aegir::nmspace::kPortNameLength);
    if (!g_control.valid() || !port.valid() || !nmspace.valid()) {
        write("      netvol: no control port, no volume port or no namespace\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The volume's caller half, minted unbadged: the VFS badges each
     * resolver's own copy. */
    uint64_t owner_slot = 0;
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
    static_cast<void>(aegir::bootstrap::capability("vol.net", 7, &owner_slot));
    seL4_CPtr const caller_half = static_cast<seL4_CPtr>(first_free);
    if (owner_slot == 0 ||
        seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, caller_half,
                        aegir::bootstrap::kCNodeBits, aegir::bootstrap::kSlotOwnCNode,
                        static_cast<seL4_CPtr>(owner_slot), aegir::bootstrap::kCNodeBits,
                        seL4_CapRights_new(1, 1, 0, 1), 0) != seL4_NoError) {
        write("      netvol: the caller half would not mint\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* Register, then serve. Net: is writable (a write reprograms the stack),
     * public (every session may read it), and a directory tree. */
    constexpr char kVolume[] = "Net";
    uint64_t out[aegir::nmspace::kNameMax / 8 + aegir::nmspace::kTypeMax / 8 + 2] = {0};
    uint32_t out_words = aegir::nmspace::pack_string(out, kVolume, sizeof(kVolume) - 1,
                                                     aegir::nmspace::kNameMax);
    out[out_words++] = aegir::nmspace::kFlagPublic;
    out_words += aegir::nmspace::pack_string(out + out_words, "NET", 3,
                                             aegir::nmspace::kTypeMax);
    uint64_t in[aegir::nmspace::kNameMax / 8 + 1] = {0};
    aegir::ipc::WordsReply const registered =
        nmspace.call_transfer(aegir::nmspace::kMethodRegister, out, out_words, caller_half,
                              in, aegir::nmspace::kNameMax / 8 + 1, nullptr);
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, caller_half,
                      aegir::bootstrap::kCNodeBits);
    char const *assigned = nullptr;
    uint32_t assigned_length = 0;
    if (registered.error != 0 || registered.count == 0 ||
        !aegir::nmspace::unpack_string(in, registered.count, aegir::nmspace::kNameMax,
                                       &assigned, &assigned_length)) {
        write("      netvol: the namespace refused the registration\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    write("      netvol: ");
    write(assigned, assigned_length);
    write(": registered, serving\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords] = {0};
        uint32_t count = 0;
        bool cap_arrived = false;
        uint32_t const method =
            port.receive_words(words, aegir::ipc::kMaxWords, &count, nullptr, &cap_arrived);
        switch (method) {
        case aegir::volume::kMethodRead:
            answer_read(port, words, count);
            break;
        case aegir::volume::kMethodReadHandle:
        case aegir::volume::kMethodReadFrame:
            /* No `open` is served, so there are no handles; a read-frame names
             * one too. Both answer nothing, and a caller falls back to a path
             * read (kMethodRead). */
            reply_nothing(port);
            break;
        case aegir::volume::kMethodList:
            answer_list(port, words, count);
            break;
        case aegir::volume::kMethodStat:
            answer_stat(port, words, count);
            break;
        case aegir::volume::kMethodMkdir:
        case aegir::volume::kMethodRemove:
        case aegir::volume::kMethodReap:
        case aegir::volume::kMethodRename:
        case aegir::volume::kMethodTruncate:
        case aegir::volume::kMethodOpen:
        case aegir::volume::kMethodWrite:
        case aegir::volume::kMethodClose:
            answer_refuse(port);
            break;
        default:
            port.reply_words(nullptr, 0);
            break;
        }
        if (cap_arrived) {
            seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                              aegir::bootstrap::kSlotReceiveCap,
                              aegir::bootstrap::kCNodeBits);
        }
    }
}
