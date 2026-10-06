/*
 * The designed badge space (specs/authority.md), without the kernel.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Aegir identity is the badge the kernel reports, and the space that badge
 * lives in is a *designed* thing: bit 62 is the user class, a user badge
 * carries its user index at bits 24..61 and a serial at bits 0..23, and a
 * system badge has neither and is the superuser. The definitions sat in
 * aegir/ipc/port.h, which includes libsel4 -- so a value that must stay
 * kernel-free, and is host-tested, could not reach them. They are here instead
 * so it can: `aegir/ipc/port.h` includes this and keeps the same names, and the
 * process registry's authority check (specs/process.md) is the first caller on
 * the host-tested side.
 */

#ifndef AEGIR_IPC_BADGE_H
#define AEGIR_IPC_BADGE_H

#include <stdint.h>

namespace aegir::ipc {

/** The designed badge space (specs/authority.md): bit 62 is the user class --
 *  a badge with it set belongs to a user, one with it clear to the system. A
 *  user badge is `kUserBadge | (user << 24) | serial`, where user is the row
 *  in the user database (the row order is part of the format's meaning, so it
 *  is stable within a build) and serial counts what the user has run. Auth
 *  mints them (specs/auth.md); system badges stay small. */
constexpr uint64_t kUserBadge = 1ULL << 62;

constexpr uint64_t make_user_badge(uint64_t user, uint64_t serial) noexcept
{
    return kUserBadge | (user << 24) | serial;
}

/** The user index a badge carries: bits 24..61. A system badge has none, and
 *  is the superuser (specs/authority.md, specs/ownership.md). */
constexpr uint64_t kUserIndexMask = 0x3fffffffffull;

constexpr uint64_t user_index(uint64_t badge) noexcept
{
    return (badge >> 24) & kUserIndexMask;
}

constexpr bool is_user_badge(uint64_t badge) noexcept
{
    return (badge & kUserBadge) != 0;
}

/** The serial a user badge carries: bits 0..23, the low part of the identity.
 *  A process that launches others reads its own serial to find the badge range
 *  it was given (specs/launch.md). */
constexpr uint64_t kUserSerialMask = 0xffffff;

constexpr uint64_t serial_of(uint64_t badge) noexcept
{
    return badge & kUserSerialMask;
}

/** The serial space one session owns: the session's badge, its terminal's and
 *  the badges that terminal hands out (commands and nested terminals) all come
 *  from one block of the user's 24-bit serial space. Auth advances its session
 *  counter by a whole stride (specs/auth.md), so no two sessions share a serial
 *  -- the VFS binds by badge and its serials are never reused. The range is a
 *  capacity table: a session that needs more grows it here, and auth's counter
 *  follows. 2^16 leaves room for a terminal's commands and several nested
 *  terminals while 256 sessions still fit the space. */
constexpr uint64_t kSessionSerialStride = 1ULL << 16;

}  // namespace aegir::ipc

#endif  // AEGIR_IPC_BADGE_H
