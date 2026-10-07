/*
 * system.cc: what the POSIX layer answers about the system itself (specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * `uname` fills musl's `struct utsname` with the words in `aegir/release.h`, so
 * that the one place which says what this system is also answers the call a
 * compiler makes to find out (specs/clang-on-aegir.md:98). The struct is the
 * kernel's: six 65-byte NUL-terminated fields, in the order sysname, nodename,
 * release, version, machine, domainname
 * (projects/musllibc/include/sys/utsname.h:10-20). It arrives as a `void *`, so
 * this file copies fields by offset rather than including a libc header to
 * describe what it fills.
 */

#include <aegir/posix/system.h>

#include <aegir/release.h>

#include <cstddef>
#include <cstring>
#include <errno.h>

namespace aegir::posix::system {

namespace {

/** One field of the struct: 65 bytes, NUL-terminated, the rest zero. A word too
 *  long for it is truncated, which is what the kernel does with the same struct
 *  rather than failing. */
constexpr size_t kFieldBytes = 65;

void put_field(unsigned char *field, char const *word) noexcept
{
    (void)std::strncpy(reinterpret_cast<char *>(field), word, kFieldBytes - 1);
    field[kFieldBytes - 1] = '\0';
}

}  // namespace

long uname(void *buffer) noexcept
{
    if (buffer == nullptr) {
        return -EFAULT;
    }
    auto *fields = static_cast<unsigned char *>(buffer);
    put_field(fields + 0 * kFieldBytes, release::kSystem);
    put_field(fields + 1 * kFieldBytes, release::kNode);
    put_field(fields + 2 * kFieldBytes, release::kVersion);
    put_field(fields + 3 * kFieldBytes, release::kDescription);
    put_field(fields + 4 * kFieldBytes, release::kMachine);
    put_field(fields + 5 * kFieldBytes, ""); /* domainname: Aegir has none */
    return 0;
}

}  // namespace aegir::posix::system
