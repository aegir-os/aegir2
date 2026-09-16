# Userland model

Status: decided (2026-09).

## Aegir is not a POSIX system

Aegir exposes its own API — message ports, handlers, devices and libraries in
the Amiga OS lineage — and does not implement a POSIX interface. A POSIX
compatibility layer may be added **later**, as an ordinary user-level service
on top of Aegir's own primitives.

This is why the userspace C library choice (musllibc) is not a POSIX
commitment:

- **`musllibc` is a C standard library, not an OS interface.** We use it for
  `memcpy`, `malloc`, `snprintf`, `qsort`, `<stdint.h>` and the rest of the
  freestanding-embedded reality — writing a general purpose OS without a C
  library would mean writing one first.
- **The POSIX-ish surface lives elsewhere.** In `projects/seL4_libs`, and it is
  optional: `libsel4muslcsys` is described upstream as "a library to support
  muslc for the root task" — it is the shim that satisfies the syscalls musl
  wants, and it is separate from musl itself. Alongside it sit
  `libsel4utils` ("a library OS"), `libsel4simple`, `libsel4vka`,
  `libsel4vspace`, `libsel4allocman` and `libsel4sync`.
- **Therefore:** we can link musl without ever exposing `open`/`read`/`fork`.
  The syscall shim is wired for the calls we choose and stubbed elsewhere.
  Adding POSIX later means filling in those handlers or adding a service — an
  additive change, not a rewrite.

`libsel4utils` is a useful starting point and a reference for capability
bookkeeping, not Aegir's API: Aegir's own interfaces are ours to define, and the
first set of them — the root task, its services and ports, and the authority
model — are specified in `specs/director.md`, `specs/services.md` and
`specs/authority.md`.

## Build consequences

- `cmake-tool/base.cmake` does `find_package(musllibc REQUIRED)` and
  `musllibc_set_environment_flags()`, so musllibc must be **present** for the
  standard seL4 build. That is a build-system requirement, not an API
  requirement.
- Whether a given binary links the `muslc` target is a per-target decision.
  Lean components may want to link only `sel4runtime` + `libsel4`.

## Open items

Resolved, or re-framed, by the first root task (M4):

- **`libsel4muslcsys` is not linked.** `apps/aegir-hello` links
  `aegir-runtime`, `libsel4`, `sel4runtime` and musl's `libc.a` with no syscall
  shim, and runs. Nothing it does needs one: the only output path it uses is
  `seL4_DebugPutChar`, a libsel4 inline wrapper for a kernel console syscall
  (`kernel/libsel4/arch_include/riscv/sel4/arch/syscalls.h`). So the point above
  cuts both ways: musl really is linked without any of the POSIX-shaped surface
  being reachable, and the shim's handler-installation API is still
  unexercised. The first service that wants musl's stdio, heap or file
  functions is what will settle it.
- **A `sel4runtime`-only binary (no `muslc`) has not been tried.** Nothing in
  the current build needs one, and
  `musllibc_set_environment_flags()` is applied globally by the cmake-tool, so
  whether a target can opt out cleanly is still open.
- **C++ standard library posture is not an open question**: freestanding C++
  (see `specs/build.md`) means no `std::` containers and no iostreams until we
  deliberately vendor a standard library. Recorded as a posture, not as
  something to find out.

## The C/C++ boundary, recorded from building it

`specs/build.md` covers freestanding C++; what building the root task added is
that **third-party C headers are not automatically usable from C++**, in three
distinct ways, all found the same way — by the build refusing:

| Header | What happens from C++ | What we do |
| --- | --- | --- |
| `sel4/assert.h` | declares `__assert_fail` without `extern "C"`, so a C++ translation unit needs a C++-linkage definition | `libs/aegir-runtime/src/assert.cc` defines it (`specs/build.md`) |
| `sel4runtime.h` (and `sel4runtime/stdint.h`) | C-only: `_Static_assert`, which C++ rejects (`projects/sel4runtime/include/sel4runtime/stdint.h:15-19`) | declare the one function needed (`sel4runtime_bootinfo`) with `extern "C"` |
| `cpio/cpio.h` | no `extern "C"` guard, so prototypes are mangled and the link fails on names the library does not define | include it inside `extern "C" { }` |

The rule this suggests for our own code: our headers are C++ and carry their own
linkage; a third-party C header is checked before it is included, not after the
link fails. It is also an argument for Aegir's own interfaces being ours
(`specs/director.md`) rather than re-exports of somebody else's.


## Threads inside a process (found in M6c)

A thread started by hand in an existing address space needs three things a process
gets for free, and the symptoms of missing them point somewhere else entirely:

- **A thread pointer (`tp`).** libsel4 reaches the IPC buffer through the TLS
  variable `__sel4_ipc_buffer` (kernel/libsel4/include/sel4/functions.h:13), so a
  thread with `tp = 0` faults on its first syscall -- including the first
  `seL4_DebugPutChar`, which looks like "the thread never ran". Give it a TLS
  block of its own (the process's image copied in, plus *its* IPC buffer pointer);
  upstream's thread setup is the reference
  (projects/seL4_libs/libsel4utils/src/thread.c:169-177).
- **A global pointer (`gp`).** The crt computes it from `__global_pointer$`, and
  code compiled the way ours is does use it: the first global access is what
  faults, at whatever address the globals would have been. `seL4_TCB_SetTLSBase`
  is *not* how either of these reaches the thread's context -- see below.
- **A stack that does not overlap the TLS block**: the block sits at the top of the
  thread's stack pages and the stack pointer starts below it.

**Set `tp` and `gp` in the user context, not beside it.** `seL4_TCB_WriteRegisters`
writes the *whole* user context. Calling `seL4_TCB_SetTLSBase` first and then
`WriteRegisters` with a zeroed context undoes it: the thread is left with `tp = 0`,
and its first access to the IPC buffer is a fault at address zero -- which reads
like a supervisor that received nothing and simply stopped. The `seL4_UserContext`
field is the one that wins:

    context.gp = <the making thread's gp>;
    context.tp = thread_pointer;         /* from sel4runtime_write_tls_image */

The boot thread's `gp` can legitimately be zero, so read it from the running thread
rather than assuming (`mv %0, gp`); the two registers are then whatever the process
already uses, which is exactly right for a thread in the same address space.

**A virtual destructor costs a symbol we do not have.** Giving a base class a
virtual destructor makes the compiler emit a deleting destructor, which needs
`operator delete` -- and a freestanding program has none, so the link fails
(`libs/aegir-devtree`'s `Tree::Visitor` is the example). An interface with a pure
virtual function and no virtual destructor is the shape that works; nothing here
deletes through a base pointer anyway.

sel4runtime's helpers are not usable from C++ (its header is C-only, and
`sel4runtime_set_tls_variable` is a macro using `typeof`), so director declares the
two functions it needs and writes the IPC buffer pointer with
`__sel4runtime_write_tls_variable`.

## A service's stack is two pages, and some of our objects are not

`aegir-spawn` gives a spawned process two pages of stack. That is enough for ordinary
functions and not enough for our own larger structures, which are sized for the kernel's
list rather than for a small process: an `Allocator` carries room for every untyped the
bootinfo could name, plus the halves that splitting creates, which is tens of kilobytes.

Director gets away with `Allocator allocator(bootinfo)` on `main`'s stack because the
root task is given the kernel's initial stack and it is large. A spawned service is not,
and putting one there fails in the least helpful way: the stack runs off the end of its
pages into unmapped memory, which arrives as a fault the supervisor reports as
"faulted on a memory access" -- indistinguishable from a bug in anything else the service
was doing. The device manager hit exactly that, with the allocator as the only new thing
in it.

So: anything in the tens of kilobytes belongs in **static storage**, and a service's
stack budget is part of what it can be asked to do. `apps/aegir-device-manager` keeps its
allocator at file scope for this reason, with the reason written next to it.
