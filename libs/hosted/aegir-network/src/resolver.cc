/*
 * The runtime's name lookup: musl's getaddrinfo, gethostbyname and
 * freeaddrinfo over Aegir's resolver (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * musl resolves a name itself: it reads /etc/hosts and /etc/resolv.conf --
 * neither of which the VFS has -- and asks a nameserver with its own DNS
 * client. Aegir's resolver is the client's instead: Sys:S/hosts first, then the
 * net stack's DNS through the socket port. So the three libc functions are
 * replaced here at link time, the way network.cc answers the socket syscalls.
 * This is the one file in aegir-network that includes a musl header -- the
 * addrinfo and hostent types live in <netdb.h> -- because a name lookup is a
 * library function and not a syscall; network.cc stays free of them.
 *
 * The hosts file is read through the runtime's own file layer (aegir-heap's
 * files.cc), so an Aegir path resolves the way any file does and no capability
 * slot is juggled here. The line grammar is aegir::resolve::hosts_line, the same
 * one the freestanding resolver uses, so a name yields the same address
 * whichever client asks. The stack's DNS is aegir::network::resolve, the port's
 * own `resolve`, so a hosted client and a freestanding one reach one answer.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <aegir/network.h>
#include <aegir/resolve.h>

namespace {

constexpr char kHostsPath[] = "Sys:S/hosts";
/* A line's ceiling, as the freestanding resolver keeps it: hosts entries are
 * short and a longer line is malformed and skipped rather than buffered. */
constexpr uint32_t kLineMax = 256;
/* How much of the file one read asks for; the volume clamps it. */
constexpr uint32_t kReadChunk = 512;
/* How long DNS is given before the lookup gives up; a name the server does not
 * know may never be answered. */
constexpr uint32_t kDnsTimeoutMs = 2000;

/* Whether `text` is a dotted quad; when it is, `*out` is the address in the
 * native word the socket port and `struct sockaddr_in` both carry (low byte
 * first, the way network.cc's read_sockaddr decodes one). */
bool numeric_host(char const *text, uint32_t *out) noexcept
{
    struct in_addr parsed;
    if (inet_pton(AF_INET, text, &parsed) != 1) {
        return false;
    }
    memcpy(out, &parsed.s_addr, sizeof(uint32_t));
    return true;
}

/* Read Sys:S/hosts and search it for `name`. False when there is no namespace,
 * no such file, or no matching line -- any of which falls through to DNS. The
 * file is read through the runtime's file layer, a chunk at a time and a line at
 * a time. */
bool hosts_lookup(char const *name, uint32_t name_length, uint32_t *address) noexcept
{
    int const fd = open(kHostsPath, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char line[kLineMax];
    uint32_t line_length = 0;
    bool overlong = false;
    bool found = false;
    char chunk[kReadChunk];
    for (;;) {
        ssize_t const got = read(fd, chunk, sizeof(chunk));
        if (got <= 0) {
            break;
        }
        for (ssize_t i = 0; i < got; ++i) {
            char const c = chunk[i];
            if (c == '\n') {
                if (!overlong && aegir::resolve::hosts_line(line, line_length, name,
                                                            name_length, address)) {
                    found = true;
                    break;
                }
                line_length = 0;
                overlong = false;
            } else if (line_length < sizeof(line) - 1) {
                line[line_length++] = c;
            } else {
                overlong = true;
            }
        }
        if (found) {
            break;
        }
    }
    if (!found && !overlong && line_length > 0) {
        found = aegir::resolve::hosts_line(line, line_length, name, name_length, address);
    }
    close(fd);
    return found;
}

/* A name to an address in that same native word: a dotted quad parsed here,
 * else the hosts file, else the stack's DNS. False when none knows the name. */
bool resolve_host(char const *name, uint32_t *address) noexcept
{
    uint32_t const length = static_cast<uint32_t>(strlen(name));
    if (length == 0) {
        return false;
    }
    if (numeric_host(name, address)) {
        return true;
    }
    if (hosts_lookup(name, length, address)) {
        return true;
    }
    uint32_t const found = aegir::network::resolve(name, length, kDnsTimeoutMs);
    if (found == 0) {
        return false;
    }
    *address = found;
    return true;
}

}  // namespace

extern "C" {

int getaddrinfo(const char *__restrict host, const char *__restrict serv,
                const struct addrinfo *__restrict hint, struct addrinfo **__restrict res)
{
    if (host == nullptr && serv == nullptr) {
        return EAI_NONAME;
    }
    int family = AF_UNSPEC;
    int flags = 0;
    int protocol = 0;
    int socktype = 0;
    if (hint != nullptr) {
        family = hint->ai_family;
        flags = hint->ai_flags;
        protocol = hint->ai_protocol;
        socktype = hint->ai_socktype;
        const int mask = AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST |
                         AI_V4MAPPED | AI_ALL | AI_ADDRCONFIG | AI_NUMERICSERV;
        if ((flags & mask) != flags) {
            return EAI_BADFLAGS;
        }
        if (family != AF_INET && family != AF_INET6 && family != AF_UNSPEC) {
            return EAI_FAMILY;
        }
    }
    if (family == AF_INET6) {
        /* The stack speaks IPv4 only; a v6 request has nothing to answer. */
        return EAI_FAMILY;
    }

    /* The address. A null host is the wildcard when the caller wants a local
     * bind (AI_PASSIVE) and the loopback otherwise. */
    uint32_t address = 0;
    if (host == nullptr) {
        if (!(flags & AI_PASSIVE)) {
            address = 0x0100007f; /* 127.0.0.1, low byte first */
        }
    } else if (flags & AI_NUMERICHOST) {
        if (!numeric_host(host, &address)) {
            return EAI_NONAME;
        }
    } else if (!resolve_host(host, &address)) {
        return EAI_NONAME;
    }

    /* The port. Only a number is a port; a service name would need
     * /etc/services, which the VFS has not. */
    uint16_t port = 0;
    if (serv != nullptr) {
        uint32_t value = 0;
        uint32_t digits = 0;
        for (char const *p = serv; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9' || digits >= 5) {
                return EAI_SERVICE;
            }
            value = value * 10 + static_cast<uint32_t>(*p - '0');
            ++digits;
        }
        if (digits == 0 || value > 65535) {
            return EAI_SERVICE;
        }
        port = static_cast<uint16_t>(value);
    }

    /* The socket types: the hint's, or the two a caller with no hint may use. */
    struct Combo {
        int socktype;
        int protocol;
    };
    Combo combos[2];
    int count = 0;
    if (socktype != 0) {
        combos[count++] = {socktype, protocol};
    } else if (protocol == IPPROTO_TCP) {
        combos[count++] = {SOCK_STREAM, protocol};
    } else if (protocol == IPPROTO_UDP) {
        combos[count++] = {SOCK_DGRAM, protocol};
    } else if (protocol != 0) {
        combos[count++] = {0, protocol};
    } else {
        combos[count++] = {SOCK_STREAM, 0};
        combos[count++] = {SOCK_DGRAM, 0};
    }

    size_t const canon_length =
        (flags & AI_CANONNAME) != 0 && host != nullptr ? strlen(host) + 1 : 0;
    size_t const bytes = sizeof(struct addrinfo) * static_cast<size_t>(count) +
                         sizeof(struct sockaddr_in) * static_cast<size_t>(count) + canon_length;
    auto *base = static_cast<struct addrinfo *>(calloc(1, bytes));
    if (base == nullptr) {
        return EAI_MEMORY;
    }
    /* One block, so freeaddrinfo below is a single free: the addrinfo array, the
     * sockaddrs, then the canonical name if the caller asked for one. */
    auto *addresses = reinterpret_cast<struct sockaddr_in *>(base + count);
    char *canon = canon_length != 0 ? reinterpret_cast<char *>(addresses + count) : nullptr;
    if (canon != nullptr) {
        memcpy(canon, host, canon_length);
    }
    for (int k = 0; k < count; ++k) {
        base[k].ai_family = AF_INET;
        base[k].ai_socktype = combos[k].socktype;
        base[k].ai_protocol = combos[k].protocol;
        base[k].ai_addrlen = sizeof(struct sockaddr_in);
        base[k].ai_addr = reinterpret_cast<struct sockaddr *>(&addresses[k]);
        base[k].ai_canonname = k == 0 ? canon : nullptr;
        base[k].ai_next = k + 1 < count ? &base[k + 1] : nullptr;
        addresses[k].sin_family = AF_INET;
        addresses[k].sin_port = htons(port);
        addresses[k].sin_addr.s_addr = address;
    }
    *res = &base[0];
    return 0;
}

void freeaddrinfo(struct addrinfo *p)
{
    free(p);
}

struct hostent *gethostbyname(const char *name)
{
    if (name == nullptr) {
        h_errno = HOST_NOT_FOUND;
        return nullptr;
    }
    uint32_t address = 0;
    if (!resolve_host(name, &address)) {
        h_errno = HOST_NOT_FOUND;
        return nullptr;
    }
    /* One result, reused: the classic hostent's storage is the library's, as
     * musl's own is. A threaded caller wants gethostbyname_r, which is musl's
     * and still goes to /etc/hosts; this is the single-threaded shape. */
    static struct hostent entry;
    static uint32_t network_address;
    static char *addresses[2];
    static char host_name[256];
    uint32_t length = static_cast<uint32_t>(strlen(name));
    if (length >= sizeof(host_name)) {
        length = sizeof(host_name) - 1;
    }
    memcpy(host_name, name, length);
    host_name[length] = '\0';
    network_address = address;
    addresses[0] = reinterpret_cast<char *>(&network_address);
    addresses[1] = nullptr;
    entry.h_name = host_name;
    entry.h_aliases = nullptr;
    entry.h_addrtype = AF_INET;
    entry.h_length = 4;
    entry.h_addr_list = addresses;
    return &entry;
}

}  // extern "C"
