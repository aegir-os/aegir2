/*
 * Aegir's minimum console.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Writes go to the serial, whose one writer is the logger (specs/console.md):
 * the runtime hands it complete lines, so a line cannot be spliced by a service
 * that became runnable in the middle of it. A process that holds no `log.main`
 * -- the root task, the logger itself, or a service given none -- writes the
 * kernel's debug console directly (seL4_DebugPutChar), which needs a kernel
 * built with CONFIG_PRINTING. No buffering beyond the line, no formatting
 * beyond what the caller does, no libc.
 */

#ifndef AEGIR_DEBUG_H
#define AEGIR_DEBUG_H

/* The C header, not <cstdint>: there is no C++ standard library here
 * (specs/build.md). */
#include <stdint.h>

namespace aegir {

/** Write a NUL-terminated string to the kernel debug console. */
void debug_write(char const *text) noexcept;

/** Write `length` bytes to the kernel debug console: a view into a file is not
 *  NUL-terminated at its end, and treating it as if it were prints whatever
 *  the file holds next. */
void debug_write(char const *text, uint32_t length) noexcept;

/** Write an unsigned decimal number to the kernel debug console. */
void debug_write_unsigned(uint64_t value) noexcept;

/** Write a number in hexadecimal, prefixed with `0x`. */
void debug_write_hex(uint64_t value) noexcept;

/** Name the logger port this process writes through.

 *  A process given `log.main` finds it by name in its bootstrap block. The root
 *  task has no such block and still creates the port, so it mints itself a
 *  caller cap and hands it here: the logger is then the serial's one writer for
 *  the root task too, and a line it wrote cannot be spliced by a service that
 *  became runnable in the middle of it (specs/console.md). A zero port clears
 *  the override. */
void set_console_port(uint64_t port) noexcept;

/** Halt this thread forever. Never returns. */
[[noreturn]] void halt() noexcept;

}  // namespace aegir

#endif  // AEGIR_DEBUG_H
