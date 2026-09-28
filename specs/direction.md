# direction: what the next services need

Status: decided (2026-09). The ordering below, so a service's groundwork is not
relitigated per service.

Aegir is about to grow services that wait: TCP/IP and a socket layer, SMBFS or
NFS, USB. They were compared against Genode -- its seL4 IPC, its session model,
its signal framework -- and the gaps that matter for them are four, in this
order.

## 1. Two allocators share one CSpace

Auth owns its CSpace in `g_objects` from `first_free` upward, and hands the
session pool in `g_session_mem` the range from `mark` (`g_objects.slot_mark()`)
to the end -- so the two **overlap from `mark`**. It works only while auth
allocates no slot after login, and the scratch window's page tables (which come
from `g_objects`) do exactly that: the third session child retyped into
`mark` itself, where the session's own objects already sat ("Untyped Retype:
Slot #2009 in destination window non-empty"). One CSpace wants one partition --
the pool allocates *down* from the top, or lives in a CNode of its own.

Behind it is a smaller defect: the scratch window's `next_` only moves on
`rewind` (`libs/freestanding/aegir-mem/src/vspace.cc`), and `Arena`'s regions are
never unmapped, so the window climbs with every spawn. It is not what failed
(the window is a gigabyte and the climb is pages), but a region map that detaches
is the right shape and belongs with the partition fix.

## 2. Readiness is rebuilt per service

`specs/signal.md`: one primitive for waking a waiter, and `con.stream`'s
blocking `read` becomes a held reply. Every service below is an event loop over
several sources -- a port, an interrupt, a client's readiness, a timer -- and
without the primitive each rebuilds that loop from raw seL4, as the console,
virtio-input and the terminal already have.

## 3. A port has no session shape

Genode's session carries a `Label` and a `Ram_quota`/`Cap_quota`
(`repos/base/include/session/session.h`). Aegir's port is a bare endpoint with a
name, so identity is a numeric badge minted from a per-session serial table --
which is why the range keeps moving as we move who spawns. Add the label first;
add the quota and a buffer dataspace when the second I/O service lands and a
packet buffer wants to be shared.

## 4. Composition is eager

A Genode client acquires a session on demand --
`Connection(env, label, ram_quota, ...)`, with the parent routing it
(`repos/base/include/base/connection.h`, `base/include/base/child.h`'s
`with_route`) -- whereas Aegir installs a fixed, named list at spawn, so every
added dependency edits every spawn site. This is the root of the repeated
rework, and it is last on purpose: do it when a **manager must start a component
chosen at runtime** -- a USB hub starting a class driver, a network stack loaded
per interface -- not before. Eager composition is predictable and cheap for a
graph we know.

## What is not on the list

One capability per message (`kernel/manual/parts/ipc.tex:160`), synchronous
call and reply, a non-blocking read beside a signal, and Aegir's Amiga-shaped
VFS (`assigns`, `C:`, one Shell window) are shared with Genode or deliberate.
They are not gaps and should not be dissolved.
