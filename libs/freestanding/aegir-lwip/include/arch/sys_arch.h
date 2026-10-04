/*
 * lwIP's operating-system abstraction for Aegir (lwip/sys.h includes this as
 * "arch/sys_arch.h" when NO_SYS is 0).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * lwIP's threaded mode needs four things an OS must supply -- counting
 * semaphores, mutexes, mailboxes, and a way to start a thread -- and this
 * header is where the *handles* are named; their implementation is the port's
 * (specs/net.md). The handles are opaque pointers, so a contract violation
 * (using a semaphore after freeing it) is caught by the validity macros
 * rather than by dereferencing garbage.
 *
 * lwIP's own sys.h declares the functions; only the handle types and the
 * validity macros belong here.
 */

#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_MBOX_NULL NULL
#define SYS_SEM_NULL  NULL

struct sys_sem;
typedef struct sys_sem *sys_sem_t;
#define sys_sem_valid(sem)             (((sem) != NULL) && (*(sem) != NULL))
#define sys_sem_valid_val(sem)         ((sem) != NULL)
#define sys_sem_set_invalid(sem)       do { if ((sem) != NULL) { *(sem) = NULL; } } while (0)
#define sys_sem_set_invalid_val(sem)   do { (sem) = NULL; } while (0)

struct sys_mutex;
typedef struct sys_mutex *sys_mutex_t;
#define sys_mutex_valid(mutex)         sys_sem_valid(mutex)
#define sys_mutex_set_invalid(mutex)   sys_sem_set_invalid(mutex)

struct sys_mbox;
typedef struct sys_mbox *sys_mbox_t;
#define sys_mbox_valid(mbox)           sys_sem_valid(mbox)
#define sys_mbox_valid_val(mbox)       sys_sem_valid_val(mbox)
#define sys_mbox_set_invalid(mbox)     sys_sem_set_invalid(mbox)
#define sys_mbox_set_invalid_val(mbox) sys_sem_set_invalid_val(mbox)

struct sys_thread;
typedef struct sys_thread *sys_thread_t;

#ifdef __cplusplus
}
#endif

#endif /* LWIP_ARCH_SYS_ARCH_H */
