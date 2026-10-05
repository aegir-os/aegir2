/*
 * Sys:S/network.manifest, parsed (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The manifest says what the machine's adapters should be at boot: the machine's
 * hostname and, per adapter, DHCP or the static addresses. It is the boot
 * manifest's shape -- `key = value`, `[section]`, `#` comments, a `format`
 * version, unknown keys are errors rather than skips (specs/services.md) -- but
 * a distinct schema, so it has its own reader.
 *
 * Two things shape the reader:
 *
 *   - **It streams.** A network manifest is read by a freestanding command,
 *     which has no heap, so the file cannot be held whole. `feed` takes arbitrary
 *     chunks, the reader keeps only the line in progress and the section's
 *     values, and each completed section is handed to a handler as it ends --
 *     there is no table of adapters and so no capacity to guess.
 *   - **The hostname comes before the sections.** DHCP option 12 carries whatever
 *     name the netif holds when `dhcp_start` runs, so the name must be applied
 *     first; a `hostname` after the first `[section]` is refused, loudly.
 */

#ifndef AEGIR_NET_CONFIG_MANIFEST_H
#define AEGIR_NET_CONFIG_MANIFEST_H

#include <stdint.h>

namespace netconfig {

/* A hostname's ceiling: DNS caps a label at 63 bytes (RFC 1035), and the DHCP
 * option 12 it rides carries a label, not a name with a domain. An adapter
 * name's ceiling: the stack answers a name up to its 8-byte word, given room to
 * be read. A line's ceiling: comfortably past the longest the schema allows
 * (`ipv4_netmask = 255.255.255.255`), and a longer line is malformed and
 * refused rather than trimmed. */
constexpr uint32_t kHostnameMax = 63;
constexpr uint32_t kNameMax = 15;
constexpr uint32_t kLineMax = 128;

/** One adapter's configuration, as its section declares it. The addresses are
 *  network order; `has_*` says whether the key was there. */
struct Adapter {
    char name[kNameMax + 1];
    uint32_t name_length;
    bool dhcp;
    bool has_address;
    uint32_t address;
    bool has_netmask;
    uint32_t netmask;
    bool has_gateway;
    uint32_t gateway;
};

struct Problem {
    uint32_t line;
    char const *message;
};

class Manifest {
public:
    /** Where a completed hostname or section goes. `context` is the caller's;
     *  the pointers are the caller's to read and not to keep. */
    struct Handler {
        void *context;
        void (*hostname)(void *context, char const *name, uint32_t length) noexcept;
        void (*adapter)(void *context, Adapter const &adapter) noexcept;
    };

    Manifest() noexcept;

    void set_handler(Handler const &handler) noexcept;

    /** Feed the manifest in chunks. False means `problem()` says why, and no
     *  handler is called after the failure. */
    bool feed(char const *text, uint32_t length) noexcept;

    /** No more text: flushes the last line and the last section. */
    bool finish() noexcept;

    Problem problem() const noexcept { return problem_; }

    /** Whether a parse failure was recorded (problem() says which). False for a
     *  file that was never read at all. */
    bool failed() const noexcept { return problem_.message != nullptr; }

private:
    bool fail(char const *message) noexcept;
    bool line(char const *begin, char const *end) noexcept;
    void emit_current() noexcept;

    Handler handler_;
    Problem problem_;

    char line_[kLineMax];
    uint32_t line_length_;
    bool overlong_;
    uint32_t line_number_;

    bool format_seen_;
    bool hostname_seen_;
    bool in_section_;
    Adapter current_;
};

}  // namespace netconfig

#endif  // AEGIR_NET_CONFIG_MANIFEST_H
