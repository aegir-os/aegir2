/*
 * Aegir's assertion failure handler.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * libsel4's headers call __assert_fail from inline code (seL4_Fail, assert),
 * and sel4/assert.h declares it *without* extern "C". In C that declaration
 * matches musl's C definition and everything links. In C++ it is a
 * C++-linkage declaration, so any C++ translation unit that uses libsel4's
 * inline helpers needs a C++-linkage definition -- which is this file.
 *
 * Forwarding to musl's C function is not an option (that is a different,
 * C-linkage symbol), and there is nothing to forward to anyway: in Aegir an
 * assertion failure is a fatal fault, so it reports and stops. That is the
 * behaviour we want regardless.
 */

#include <aegir/debug.h>

#include <sel4/sel4.h>

namespace {

void write_unsigned(seL4_Word value) noexcept
{
    char digits[20];
    int length = 0;
    do {
        digits[length++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && length < static_cast<int>(sizeof(digits)));
    while (length > 0) {
        seL4_DebugPutChar(digits[--length]);
    }
}

}  // namespace

// Signature and linkage must match sel4/assert.h as seen from C++. No
// [[noreturn]]: the upstream declaration does not have it, and disagreeing
// about it buys nothing here.
void __assert_fail(char const *assertion, char const *file, int line, char const *function)
{
    aegir::debug_write("\nAEGIR PANIC: assertion failed: ");
    aegir::debug_write(assertion != nullptr ? assertion : "(null)");
    aegir::debug_write("\n  at ");
    aegir::debug_write(file != nullptr ? file : "(unknown file)");
    aegir::debug_write(":");
    write_unsigned(static_cast<seL4_Word>(line));
    aegir::debug_write(" in ");
    aegir::debug_write(function != nullptr ? function : "(unknown function)");
    aegir::debug_write("\n");
    aegir::halt();
}
