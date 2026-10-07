/*
 * aegir/posix/system.h: what the layer answers when a program asks about the
 * system itself (specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * `uname` is the call a compiler makes to find out what it is running on -- LLVM
 * reads it to choose a target and a host, and specs/clang-on-aegir.md:98 recorded
 * it as a stub before this -- so the words it answers are the ones in
 * `aegir/release.h`: one place holds what this system says it is, and this call
 * is how a program hears it.
 *
 * The buffer is musl's own `struct utsname`, which is the kernel's: six 65-byte
 * fields, in the order sysname, nodename, release, version, machine, domainname
 * (projects/musllibc/include/sys/utsname.h:10-20). The C surface here takes a
 * `void *` the way the file calls do, so the layer never depends on a libc header
 * to describe what it fills.
 */

#pragma once

namespace aegir::posix::system {

/** Fill a `struct utsname` (the six 65-byte fields above). Returns 0, or a
 *  negative errno. */
long uname(void *buffer) noexcept;

}  // namespace aegir::posix::system
