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
