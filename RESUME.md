# Task: the pager landed — the next arc

Repo: `/home/rroland/src/aegir2` (branch `main`). The pager is **in**, and the
on-device C++ compiler is green: `make run TARGET=aegir-8g-smp4` passes with
`CXX_SMOKE_OK` and `AEGIR_HELLO_OK` (`PASS aegir-8g-smp4 booted
(AEGIR_BOOT_OK)`), no `<<seL4>>` line, no fault. This file is the handoff for
whatever comes next, and the record of the two side-findings the pager arc
turned up.

## What landed (each its own commit)

| commit | what |
|---|---|
| `e4f30f98` | step 1 — stack above a guard page; block and IPC above the stack |
| `1bd5deac` | merge `wip/compiler-sysroot` (the C++ acceptance, the sysroot, stat identity) |
| `595f8923` | drop the stray `<bits/stdint.h>` probe from the acceptance |
| `731fcae7` | step 2 — read `PT_GNU_STACK`, hosted link flag, `stack_max_bytes` |
| `f03be6a7` | step 3a — reserve the whole stack range, map only the first pages |
| `7060b786` | step 3b — the launcher's pager: grow on a fault, child-badged frames, end status 129 |
| `cf812895` | the pager retypes into a command's own CNode run |
| `538d53c2` | `mkfs_bfs` multi-level directory offsets — **this is what finished clang** |
| `03df3959` | musl: `PT_GNU_STACK` is the process stack, not a thread default |
| `03d0f9de` | the compile's proof: `dir SCRATCH:`, an anchored cue, so a missing object fails the run |
| `0078115d` | share the stack-growth walk between the spawners (`aegir-spawn`'s `page_stack`) |
| `7bc5eea4` | director's pager: a reserved pool, child-badged `mem.main` chunks, `watched` gated on `mem.main` |
| `94a7350f` | an acceptance for the pager's named stack overflow (`overflow`, 128 KiB max) |

The pager grew the compiler's stack on demand (measured: 19 pages, then one
128 KiB step through the per-command run — `pager run used=32 lo=0x4fde000
base=12 cnodes=1`), and director's pager grew a forced boot service (32 carves
from a child-badged 2 MiB `mem.main` chunk). `specs/pager.md` is the decision;
the resolution order is launch request > `stack_kib` > `PT_GNU_STACK.p_memsz` >
default (16 pages initial, 8 MiB max, 128 KiB step).

## The two side-findings worth carrying

- **`mkfs_bfs._tree`'s internal-node offsets were off by one** (`NODE * (2 +
  len(nodes) + n)`; a node at list index `i` is written at `NODE * (i + 1)`).
  A directory that split into two leaves had `root == maximum_size`, so the
  guest's `dir_find` refused every lookup in it. That is the staged sysroot's
  `__algorithm`: `<string>` includes `<__algorithm/max.h>` and could not find
  it, while `c++/v1` (one leaf) resolved `string`. **A host case for a
  multi-leaf directory is still missing**, and this class of bug is invisible
  until a directory grows past one 4096-byte node.
- **musl reads `PT_GNU_STACK.p_memsz` for `__default_stacksize`**
  (`projects/musl/src/env/__init_tls.c:98`). Aegir's spawner reads the same
  field as the process's maximum stack, so the link flag made every
  `std::thread` ask for the whole declared stack; a boot service's single-level
  CSpace filled (`slots 4084 of 4084`) and the cxx smoke died of `EAGAIN`. Now a
  tracked musl patch (`third_party/patches/projects/musl/0004-…`) keeps
  `__default_stacksize` at `DEFAULT_STACK_SIZE`. Thread-stack policy is Aegir's
  to choose from here.

## What is left, in rough order

1. **File-backed paging** (step 4): the ELF segments through the same
   `populate(vspace, addr) -> source` interface, so an 85 MB image is not mapped
   to touch 5 MB of it.
2. **The used-slot wall** (`specs/userland.md`, "a used slot is still
   allocation-only"): a process that maps and unmaps still fills its CNode. The
   musl patch sidesteps it for thread stacks; a growable CSpace for boot
   services is the general fix (`specs/memory.md`'s "Growing the CSpace on
   demand").
3. **The root's two-level CSpace** (optional, for uniformity): director is the
   one CSpace with the kernel's initial guard/radix convention. Measured, the
   re-root's `l2` would have to be >= 15 (director holds 22368 caps), so
   per-service CNodes would be ~512 KB each — the pager does not need it (it has
   its own pool), so this wants deciding on its own merits.
4. **Wire `check-bfs`** (the multi-leaf case from finding 1) as a prerequisite of
   `build`/`run`, the way `check-allocator` is.

## Read first

- `specs/pager.md` — the decision and what landed.
- `specs/memory.md` — the memory service, grants badged to a child, `release`.
- `specs/launch.md` — the spawn request and the fault supervisor.
- `specs/userland.md` — "a service's stack is what the spawner gives it", and the
  used-slot wall.
- `third_party/patches/README.md` — the patch rules (the musl patch is tracked).
- `AGENTS.md`.

## Constraints (unchanged)

- Builds/runs under `timeout`; check for orphaned `qemu-system` afterward.
- No `<<seL4 …>>` line, no `command faulted` on a green run.
- Never edit a vendored tree in place; patches under `third_party/patches/` are
  the source of truth, and `make deps-check` must pass.
- A check that cannot fail the command is not a check.
- STOP and ask rather than changing the plan.
