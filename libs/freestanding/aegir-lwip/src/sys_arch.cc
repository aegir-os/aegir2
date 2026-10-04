/*
 * lwIP's threading and blocking primitives for Aegir.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * NO_SYS=0 needs four things an OS supplies: counting semaphores, mutexes,
 * mailboxes, and a way to start a thread (specs/net.md). Each blocking object
 * here is an seL4 **notification** retyped from the service's own untyped, and
 * waiting on one is `seL4_Wait`. lwIP's `sys_sem_new(count)` count is 0 or 1,
 * so a notification's coalescing is harmless; a mailbox is a pointer ring whose
 * post signals that notification and whose fetch waits on it.
 *
 * The tcpip thread is the one thread lwIP starts, and the one place a timeout
 * matters: `sys_arch_mbox_fetch` waits on the mailbox *or* the timer tick, which
 * the service's tick notification (bound to that thread) delivers. A wake whose
 * badge is the tick is a timeout; every other wake is a mailbox message. The
 * other blocking objects wait forever, which is what lwIP asks of them.
 *
 * `sys_arch_protect` is a no-op: the tcpip thread owns lwIP and other threads
 * only post to its mailbox, so there is no lwIP datum two threads touch -- the
 * model lwIP's threaded API is built on.
 */

#include <aegir/lwip/port.h>

extern "C" {
#include <lwip/err.h>
#include <lwip/mem.h>
#include <lwip/sys.h>
}

#include <sel4/sel4.h>
#include <stddef.h>
#include <stdint.h>

namespace {

aegir::mem::Allocator *g_objects = nullptr;
aegir::mem::Account *g_account = nullptr;
aegir::thread::Builder *g_builder = nullptr;
aegir::thread::Placement const *g_placement = nullptr;
seL4_CPtr g_thread_tcb = 0;
volatile uint32_t g_now_ms = 0;

seL4_CPtr new_notification() noexcept
{
    if (g_objects == nullptr || g_account == nullptr) {
        return 0;
    }
    seL4_Error error = seL4_NoError;
    return g_objects->alloc_object(seL4_NotificationObject, seL4_NotificationBits,
                                   *g_account, &error);
}

}  // namespace

/* The opaque handles arch/sys_arch.h names. */
struct sys_sem {
    seL4_CPtr notification;
};
struct sys_mutex {
    seL4_CPtr notification;
};
struct sys_mbox {
    seL4_CPtr notification;
    void **ring;
    uint32_t size;
    volatile uint32_t count;
    uint32_t head;
    uint32_t tail;
};
struct sys_thread {
    seL4_CPtr tcb;
};

namespace aegir::lwip {

void set_objects(aegir::mem::Allocator &objects, aegir::mem::Account &account) noexcept
{
    g_objects = &objects;
    g_account = &account;
}

void set_threading(aegir::thread::Builder &builder,
                   aegir::thread::Placement const &placement) noexcept
{
    g_builder = &builder;
    g_placement = &placement;
}

void set_now(uint32_t milliseconds) noexcept
{
    g_now_ms = milliseconds;
}

seL4_CPtr thread_tcb() noexcept
{
    return g_thread_tcb;
}

}  // namespace aegir::lwip

extern "C" void sys_init(void)
{
}

/* Semaphores: a notification, pre-signaled when the initial count is 1. */

extern "C" err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    if (sem == nullptr) {
        return ERR_ARG;
    }
    auto *const handle = static_cast<sys_sem *>(mem_malloc(sizeof(sys_sem)));
    if (handle == nullptr) {
        *sem = nullptr;
        return ERR_MEM;
    }
    handle->notification = new_notification();
    if (handle->notification == 0) {
        mem_free(handle);
        *sem = nullptr;
        return ERR_MEM;
    }
    if (count != 0) {
        seL4_Signal(handle->notification);
    }
    *sem = handle;
    return ERR_OK;
}

extern "C" void sys_sem_signal(sys_sem_t *sem)
{
    if (sem != nullptr && *sem != nullptr) {
        seL4_Signal((*sem)->notification);
    }
}

extern "C" u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    (void)timeout; /* lwIP asks forever of every semaphore it waits on */
    if (sem == nullptr || *sem == nullptr) {
        return SYS_ARCH_TIMEOUT;
    }
    seL4_Wait((*sem)->notification, nullptr);
    return 1;
}

extern "C" void sys_sem_free(sys_sem_t *sem)
{
    if (sem != nullptr && *sem != nullptr) {
        mem_free(*sem);
        *sem = nullptr;
    }
}

/* Mutexes: a binary notification, signaled at creation (unlocked). */

extern "C" err_t sys_mutex_new(sys_mutex_t *mutex)
{
    if (mutex == nullptr) {
        return ERR_ARG;
    }
    auto *const handle = static_cast<sys_mutex *>(mem_malloc(sizeof(sys_mutex)));
    if (handle == nullptr) {
        *mutex = nullptr;
        return ERR_MEM;
    }
    handle->notification = new_notification();
    if (handle->notification == 0) {
        mem_free(handle);
        *mutex = nullptr;
        return ERR_MEM;
    }
    seL4_Signal(handle->notification); /* unlocked */
    *mutex = handle;
    return ERR_OK;
}

extern "C" void sys_mutex_lock(sys_mutex_t *mutex)
{
    if (mutex != nullptr && *mutex != nullptr) {
        seL4_Wait((*mutex)->notification, nullptr);
    }
}

extern "C" void sys_mutex_unlock(sys_mutex_t *mutex)
{
    if (mutex != nullptr && *mutex != nullptr) {
        seL4_Signal((*mutex)->notification);
    }
}

extern "C" void sys_mutex_free(sys_mutex_t *mutex)
{
    if (mutex != nullptr && *mutex != nullptr) {
        mem_free(*mutex);
        *mutex = nullptr;
    }
}

/* Mailboxes: a ring of pointers, a notification for the wake. The ring crosses
 * two cores (a netif input thread posts, the tcpip thread fetches), so the
 * count is updated behind a fence and the producer waits for room. */

extern "C" err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    if (mbox == nullptr) {
        return ERR_ARG;
    }
    auto *const handle = static_cast<sys_mbox *>(mem_malloc(sizeof(sys_mbox)));
    if (handle == nullptr) {
        *mbox = nullptr;
        return ERR_MEM;
    }
    uint32_t const capacity = size > 0 ? static_cast<uint32_t>(size) : 1u;
    handle->ring = static_cast<void **>(mem_malloc(sizeof(void *) * capacity));
    handle->notification = new_notification();
    if (handle->ring == nullptr || handle->notification == 0) {
        mem_free(handle->ring);
        mem_free(handle);
        *mbox = nullptr;
        return ERR_MEM;
    }
    handle->size = capacity;
    handle->count = 0;
    handle->head = 0;
    handle->tail = 0;
    *mbox = handle;
    return ERR_OK;
}

extern "C" void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    if (mbox == nullptr || *mbox == nullptr) {
        return;
    }
    sys_mbox *const handle = *mbox;
    while (handle->count >= handle->size) {
        seL4_Yield(); /* full: give the consumer its core, then try again */
    }
    handle->ring[handle->tail] = msg;
    handle->tail = (handle->tail + 1) % handle->size;
    __sync_synchronize();
    handle->count = handle->count + 1;
    seL4_Signal(handle->notification);
}

extern "C" err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    if (mbox == nullptr || *mbox == nullptr) {
        return ERR_MEM;
    }
    sys_mbox *const handle = *mbox;
    if (handle->count >= handle->size) {
        return ERR_MEM;
    }
    handle->ring[handle->tail] = msg;
    handle->tail = (handle->tail + 1) % handle->size;
    __sync_synchronize();
    handle->count = handle->count + 1;
    seL4_Signal(handle->notification);
    return ERR_OK;
}

/* The interrupt-context form lwIP's tcpip.c references: the same as trypost
 * here, because a mailbox post never blocks -- the caller is told the mailbox
 * was full rather than waiting on it. */
extern "C" err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    return sys_mbox_trypost(mbox, msg);
}

extern "C" u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    (void)timeout;
    if (mbox == nullptr || *mbox == nullptr) {
        return SYS_ARCH_TIMEOUT;
    }
    sys_mbox *const handle = *mbox;
    while (handle->count == 0) {
        seL4_Word badge = 0;
        seL4_Wait(handle->notification, &badge);
        if (badge == aegir::lwip::kTickBit) {
            return SYS_ARCH_TIMEOUT; /* the timer tick: run lwIP's timers */
        }
    }
    __sync_synchronize();
    if (msg != nullptr) {
        *msg = handle->ring[handle->head];
    }
    handle->head = (handle->head + 1) % handle->size;
    handle->count = handle->count - 1;
    return 1;
}

extern "C" u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    if (mbox == nullptr || *mbox == nullptr) {
        return SYS_MBOX_EMPTY;
    }
    sys_mbox *const handle = *mbox;
    if (handle->count == 0) {
        return SYS_MBOX_EMPTY;
    }
    __sync_synchronize();
    if (msg != nullptr) {
        *msg = handle->ring[handle->head];
    }
    handle->head = (handle->head + 1) % handle->size;
    handle->count = handle->count - 1;
    return 0;
}

extern "C" void sys_mbox_free(sys_mbox_t *mbox)
{
    if (mbox == nullptr || *mbox == nullptr) {
        return;
    }
    sys_mbox *const handle = *mbox;
    mem_free(handle->ring);
    mem_free(handle);
    *mbox = nullptr;
}

/* The one thread lwIP starts (the tcpip thread), through the service's thread
 * builder. lwIP starts exactly one, so one handle stands. */

extern "C" sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg,
                                       int stacksize, int prio)
{
    (void)name;
    (void)stacksize;
    (void)prio;
    static sys_thread handle;
    if (g_builder == nullptr || g_placement == nullptr) {
        return nullptr;
    }
    aegir::thread::Thread built{};
    if (!g_builder->start(*g_placement, thread, arg, built)) {
        return nullptr;
    }
    handle.tcb = built.tcb;
    g_thread_tcb = built.tcb;
    return &handle;
}

/* Time and protection. `sys_now` is the cached count the service advances from
 * its tick; protection is the no-op the tcpip-serialized model allows. */

extern "C" u32_t sys_now(void)
{
    return g_now_ms;
}

extern "C" sys_prot_t sys_arch_protect(void)
{
    return 0;
}

extern "C" void sys_arch_unprotect(sys_prot_t pval)
{
    (void)pval;
}
