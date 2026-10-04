/*
 * lwIP's compiler and platform abstraction for Aegir (lwip/arch.h includes
 * this as "arch/cc.h").
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Three things live here and nowhere else, because lwip/arch.h says so: the
 * byte order (RISC-V is little-endian, lwIP's own default), the random-number
 * function, and how a diagnostic and an assertion reach the world.
 *
 * The three hooks are *declared* here and implemented by the port's port.cc,
 * which is the service's side of the boundary (specs/net.md): a diagnostic
 * goes to Aegir's line writer, and an assertion stops the thread rather than
 * letting a broken invariant travel on the wire. They are declared, not
 * defined, because the arch header is included by lwIP's C sources and the
 * implementation is C++ that reaches aegir-runtime.
 */

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stddef.h>
#include <stdint.h>

/* The value sys_arch_protect() saves and sys_arch_unprotect() restores: the
 * interrupt state of the calling core. Delivered by the port's sys_arch. */
typedef uint32_t sys_prot_t;

void aegir_lwip_diag(const char *format, ...);
void aegir_lwip_assert(const char *message, const char *file, int line);
uint32_t aegir_lwip_rand(void);

/* lwIP calls LWIP_PLATFORM_DIAG(("format", args)) -- the argument is a
 * parenthesised printf argument list, so the macro expands to a call. */
#define LWIP_PLATFORM_DIAG(x)   do { aegir_lwip_diag x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { aegir_lwip_assert((x), __FILE__, __LINE__); } while (0)

/* DHCP transaction ids, TCP initial sequence numbers and the like. The port
 * answers from the machine's entropy source (specs/net.md); lwIP's default of
 * calling rand() has no seed on a freestanding target, so it is replaced. */
#define LWIP_RAND() (aegir_lwip_rand())

#endif /* LWIP_ARCH_CC_H */
