/*
 * lwIP's platform hooks for Aegir: lwip/arch.h includes arch/cc.h, which
 * declares these three, and this file is their body.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A diagnostic goes to Aegir's line writer (aegir::debug_write) rather than the
 * C library's printf-and-flush that lwIP's default pulls in; an assertion says
 * what broke and then stops the thread, because a network invariant that the
 * stack carries on past is worse than a stop. The random-number hook feeds
 * lwIP's DHCP transaction ids and TCP initial sequence numbers: it is seeded by
 * the service from the machine's entropy source (rng.virtio0) before lwIP
 * starts, and an unseeded one is a fixed sequence -- predictable, which matters
 * precisely where lwIP uses it, so the service must seed it.
 */

#include <aegir/debug.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

namespace {
uint64_t g_random = 0x9e3779b97f4a7c15ull; /* a nonzero default, overwritten by the seed */
}  // namespace

extern "C" void aegir_lwip_diag(const char *format, ...)
{
    char line[160];
    va_list arguments;
    va_start(arguments, format);
    int const written = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (written > 0) {
        uint32_t const length = static_cast<uint32_t>(written) < sizeof(line) - 1
                                    ? static_cast<uint32_t>(written)
                                    : sizeof(line) - 1;
        aegir::debug_write("      lwip: ");
        aegir::debug_write(line, length);
        aegir::debug_write("\n");
    }
}

extern "C" void aegir_lwip_assert(const char *message, const char *file, int line)
{
    aegir::debug_write("      lwip: assertion \"");
    aegir::debug_write(message);
    aegir::debug_write("\" at ");
    aegir::debug_write(file);
    aegir::debug_write(":");
    aegir::debug_write_unsigned(static_cast<uint64_t>(line));
    aegir::debug_write("\n");
    /* Stop this thread without spinning: a throwing assertion that burns a core
     * at a service's priority starves everything else on it, which looks like a
     * wedged machine rather than the one thread that broke. */
    aegir::halt();
}

/** Seed the random hook: the service calls this once, before lwIP starts, from
 *  the entropy source. */
extern "C" void aegir_lwip_rand_seed(uint64_t seed)
{
    g_random = seed != 0 ? seed : 0x9e3779b97f4a7c15ull;
}

extern "C" uint32_t aegir_lwip_rand(void)
{
    /* xorshift64: cheap, and the seed is the entropy (the service's). */
    g_random ^= g_random >> 12;
    g_random ^= g_random << 25;
    g_random ^= g_random >> 27;
    return static_cast<uint32_t>((g_random * 0x2545f4914f6cdd1dull) >> 32);
}
