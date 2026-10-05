/*
 * Sys:S/network.manifest's parser, against its host conformance cases.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Driven by scripts/check_netmanifest.py, which compiles this beside the app's
 * manifest.cc and a stub of aegir/resolve.h. It checks what the parser owns: the
 * schema's keys and values, the loud refusals, the `hostname`-before-sections
 * rule, and -- the reason the reader exists in this shape -- that feeding it in
 * chunks (here one byte at a time) gives the same result as feeding it whole.
 */

#include "manifest.h"

#include <aegir/resolve.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

/* The one resolver function the parser uses. A straightforward, correct reader,
 * so a failing case is the parser's. */
namespace aegir::resolve {

uint32_t parse_ipv4(char const *text) noexcept
{
    uint32_t value = 0;
    uint32_t part = 0;
    uint32_t parts = 0;
    uint32_t digits = 0;
    for (char const *p = text;; ++p) {
        if (*p >= '0' && *p <= '9') {
            part = part * 10 + static_cast<uint32_t>(*p - '0');
            if (part > 255 || ++digits > 3) {
                return 0;
            }
        } else if (*p == '.' || *p == '\0') {
            if (digits == 0 || parts > 3) {
                return 0;
            }
            value |= part << (8 * parts);
            ++parts;
            part = 0;
            digits = 0;
            if (*p == '\0') {
                break;
            }
        } else {
            return 0;
        }
    }
    return parts == 4 ? value : 0;
}

}  // namespace aegir::resolve

namespace {

int failures = 0;

void check(bool condition, char const *what)
{
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

struct Sink {
    char hostname[64];
    uint32_t hostname_length;
    int hostname_calls;
    netconfig::Adapter adapters[16];
    int adapter_count;
};

void on_hostname(void *context, char const *name, uint32_t length) noexcept
{
    auto *sink = static_cast<Sink *>(context);
    sink->hostname_length = length < sizeof(sink->hostname) ? length : sizeof(sink->hostname);
    for (uint32_t i = 0; i < sink->hostname_length; ++i) {
        sink->hostname[i] = name[i];
    }
    ++sink->hostname_calls;
}

void on_adapter(void *context, netconfig::Adapter const &adapter) noexcept
{
    auto *sink = static_cast<Sink *>(context);
    if (sink->adapter_count < 16) {
        sink->adapters[sink->adapter_count++] = adapter;
    }
}

/* Feed `text` one byte at a time -- the streaming path -- and answer finish's
 * verdict. */
bool parse(const char *text, Sink &sink, bool *failed)
{
    netconfig::Manifest manifest;
    netconfig::Manifest::Handler const handler{&sink, on_hostname, on_adapter};
    manifest.set_handler(handler);
    uint32_t const length = static_cast<uint32_t>(std::strlen(text));
    for (uint32_t i = 0; i < length; ++i) {
        if (!manifest.feed(text + i, 1)) {
            *failed = manifest.failed();
            return false;
        }
    }
    if (!manifest.finish()) {
        *failed = manifest.failed();
        return false;
    }
    *failed = manifest.failed();
    return true;
}

bool valid(const char *text, Sink &sink)
{
    sink = Sink{};
    bool failed = true;
    bool const ok = parse(text, sink, &failed);
    check(ok && !failed, "a valid manifest was accepted");
    return ok;
}

bool rejected(const char *text)
{
    Sink sink{};
    bool failed = false;
    bool const ok = parse(text, sink, &failed);
    check(!ok, "a malformed manifest was refused");
    check(failed, "the refusal recorded a problem");
    return !ok;
}

char const kGood[] =
    "# a machine\n"
    "format = 1\n"
    "hostname = aegir\n"
    "\n"
    "[NE0]\n"
    "dhcp = true\n"
    "\n"
    "[NE1]\n"
    "ipv4_address = 10.0.2.15\n"
    "ipv4_netmask = 255.255.255.0\n"
    "ipv4_gateway = 10.0.2.2\n";

}  // namespace

int main()
{
    Sink sink;
    if (valid(kGood, sink)) {
        check(sink.hostname_calls == 1, "the hostname was handed on once");
        check(sink.hostname_length == 5 &&
                  std::memcmp(sink.hostname, "aegir", 5) == 0,
              "the hostname's bytes");
        check(sink.adapter_count == 2, "two adapter sections");
        check(std::strcmp(sink.adapters[0].name, "NE0") == 0, "the first name");
        check(sink.adapters[0].dhcp, "NE0 asked for dhcp");
        check(!sink.adapters[0].has_address, "NE0 has no static address");
        check(std::strcmp(sink.adapters[1].name, "NE1") == 0, "the second name");
        check(!sink.adapters[1].dhcp, "NE1 did not ask for dhcp");
        check(sink.adapters[1].has_address &&
                  sink.adapters[1].address == 0x0F02000Au /* 10.0.2.15 network order */,
              "NE1's address");
        check(sink.adapters[1].has_netmask &&
                  sink.adapters[1].netmask == 0x00FFFFFFu /* 255.255.255.0 */,
              "NE1's netmask");
        check(sink.adapters[1].has_gateway &&
                  sink.adapters[1].gateway == 0x0202000Au /* 10.0.2.2 */,
              "NE1's gateway");
    }

    /* A section with no trailing newline and no keys still ends and is handed on. */
    {
        Sink s;
        valid("format = 1\n[NE0]", s);
        check(s.adapter_count == 1, "a section at end of file is emitted");
    }

    /* The hostname is optional. */
    {
        Sink s;
        valid("format = 1\n[NE0]\ndhcp = true\n", s);
        check(s.hostname_calls == 0, "no hostname means no call");
    }

    /* Loud refusals. */
    rejected("hostname = aegir\n");                       /* no format */
    rejected("format = 2\n");                             /* wrong version */
    rejected("format = 1\nbinry = aegir\n");              /* unknown top-level key */
    rejected("format = 1\nhostname = aegir\n[NE0]\nhostnam = x\n"); /* unknown key */
    rejected("format = 1\n[NE0]\ndhcp = maybe\n");        /* not a boolean */
    rejected("format = 1\n[NE0]\nipv4_address = 10.0.2.999\n"); /* bad quad */
    rejected("format = 1\n[NE0]\ndhcp = true\nhostname = aegir\n"); /* hostname too late */
    rejected("format = 1\nhostname = aegir\nhostname = other\n");   /* twice */

    /* A line longer than the reader holds is refused, not trimmed. */
    {
        char long_line[400];
        std::strcpy(long_line, "format = 1\nhostname = ");
        for (int i = 0; i < 200; ++i) {
            std::strcat(long_line, "a");
        }
        std::strcat(long_line, "\n");
        rejected(long_line);
    }

    if (failures == 0) {
        std::printf("netmanifest: all cases passed\n");
        return 0;
    }
    std::printf("netmanifest: %d case(s) failed\n", failures);
    return 1;
}
