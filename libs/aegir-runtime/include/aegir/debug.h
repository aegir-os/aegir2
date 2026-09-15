/*
 * Aegir's minimum console.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Writes go to the kernel's debug console (seL4_DebugPutChar), which needs a
 * kernel built with CONFIG_PRINTING. This is deliberately the smallest useful
 * thing: no buffering, no formatting beyond what the caller does, no libc. A
 * real console service comes later; this exists so that early userland and our
 * own assertion failures have somewhere to speak.
 */

#ifndef AEGIR_DEBUG_H
#define AEGIR_DEBUG_H

namespace aegir {

/** Write a NUL-terminated string to the kernel debug console. */
void debug_write(char const *text) noexcept;

/** Halt this thread forever. Never returns. */
[[noreturn]] void halt() noexcept;

}  // namespace aegir

#endif  // AEGIR_DEBUG_H
