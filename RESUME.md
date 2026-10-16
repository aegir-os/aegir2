# Task: give Aegir a pager — demand-grown stacks, the seed of paging

Repo: `/home/rroland/src/aegir2` (branch `main`). Aegir has no paging, and the
on-device compiler just died of it: it overflowed a 64 KiB stack that
`aegir-spawn` had mapped eagerly, with the process's own IPC buffer directly
below it, so the overflow silently clobbered the IPC buffer and then took a
VMFault in the bootstrap block (see `specs/pager.md`, "Why, measured"). Build the
**pager** — the task that serves a thread's fault endpoint and maps the page it
faulted on — starting with the stack. The decision and its reasoning are
`specs/pager.md`; **none of it is implemented yet.**

## Where the tree is

```
make build TARGET=aegir-8g-smp4     # BUILD_TIMEOUT=1800
make run   TARGET=aegir-8g-smp4     # RUN_TIMEOUT=900
```

`make run` is currently red at the compiler's compile step — that is the bug this
task fixes. It gets as far as `/Sys/Development/C/clang` running, resolving the
sysroot, and faulting (see below).

**The tree is clean, and the compiler-with-headers work is parked on a branch.**
The investigation probes have been stripped, and the rest of the previous
session's WIP moved to:

```
wip/compiler-sysroot    the on-device sysroot + real stat identity (below)
```

`main` (where you are) carries this plan — `specs/pager.md` and this file — plus
the launcher's fault detail (below). **Do not merge `wip/compiler-sysroot` until
the stack is fixed:** its acceptance is exactly the overflow this task lands.

### In the tree: the launcher's fault detail

`apps/hosted/aegir-launcher/src/main.cc` reports the fault's kind and, for a
VMFault, its IP / address / FSR. It is *not* on the branch, because step 3
extends it into the stack-overflow report. `fault 5` alone sent the previous
session down the wrong path for hours; keep it, and make the pager print more.

### Parked: the compiler-with-headers WIP (`wip/compiler-sysroot`)

The other half of "a real clang with headers on Aegir" (`specs/development.md`).
It builds clean; the guest run is red only because of the stack. When the pager
is in, merge it and finish the acceptance:

- `scripts/run_target.py` stages the sysroot headers into `Sys:Development/Include`
  (musl + libc++ + Aegir libs + seL4) and generates `Sys:Development/C/clang.cfg`
  carrying the include path **and** the library path / crt / archives.
  `scripts/sysroot-hello.c` → `scripts/sysroot-hello.cc` (a C++ program including
  `<unistd.h>`, libc++ and a Trinket header). The acceptance types
  `Sys:Development/C/clang -c /Sys/Development/hello.cc …` and links via the
  config (no runner-built link command).
- The triple's stat identity: `aegir-volume/include/aegir/volume.h`
  (`kStatTailWords` now 4, with the inode), `aegir-vfs-client`'s `Info.ino` and
  its read, the BFS/FAT/initrd/9p `stat` answers, `libs/freestanding/aegir-9p`'s
  qid path, and `aegir-posix`'s `fill_kstat` (which used to hardcode
  `st_ino = 1`, collapsing every include dir to one — clang's directory dedup).
  `specs/vfs.md`'s stat bullet documents the new word.
- `apps/hosted/aegir-clang/src/main.cc` presents `argv[0]` in the POSIX view
  before seeding LLVM (so the driver's install dir is `/Sys/…`, not a native
  path LLVM's `make_absolute` mangles).
- `libs/aegir-posix/src/spawn.cc` translates a POSIX program path to the native
  form before the launcher sees it.

## The decision (settled — do not relitigate)

`specs/pager.md` has it in full. In one breath:

1. **Layout + guard.** Move the bootstrap block and IPC buffer **above** the
   stack; keep an **unmapped guard page below** it. An overflow then faults at the
   stack's own boundary instead of corrupting the IPC buffer.
2. **Grow on demand, bounded.** Reserve the stack range, map the initial pages,
   grow down on a fault in the range. FreeBSD's numbers: 16 pages initial mapped
   (the existing `kDefaultStackPages`), 8 MiB max (`DFLSSIZ`), 512 MiB hard
   (`MAXSSIZ`), 128 KiB step (`SGROWSIZ`), 1 guard page.
3. **The pager is the spawner** (director for services, launcher for commands). It
   retains the child's **VSpace root** — its only new authority — and buys frames
   from **`memory`** through a `mem.main` copy **badged with the child's id**, so
   `release(child_badge)` reaps the stack with everything else.
4. **An overflow is a named, deliberate end:** a distinct end status, reported by
   name with `pc`/`sp`/`base`/`max`, terminated through the supervisor's existing
   reap — never a silent corruption.
5. **The declaration is `PT_GNU_STACK.p_memsz`,** set by the standard
   `-Wl,-z,stack-size=<bytes>` (the prebuilt host `ld.lld` supports it; Aegir's
   binaries already carry a `GNU_STACK` with `memsz = 0`). Resolution order:
   launch request > boot manifest (`stack_kib`) > `PT_GNU_STACK` > default. No
   custom note/object/LLVM patch for the common case.

## The plan — land each step as its own commit, re-run green each time

1. **Layout + guard.** `aegir-spawn/src/process.cc`: put `block_at`/`ipc_at`
   above the stack, add the guard page below. This alone turns the current
   "smash surprise" into one clean fault.
2. **`PT_GNU_STACK` read + the link flag.** The spawner reads `p_memsz` from the
   ELF's program headers; the hosted policy passes `-Wl,-z,stack-size=`; the
   resolution order above.
3. **The pager.** Reserve the range; the supervisor retains the child's VSpace
   root; grow on a fault in the range with frames from `memory` (child-badged);
   the distinct overflow end status; report and terminate past the bound.
4. **File-backed paging** (ELF segments), later, through the same
   `populate(vspace, addr) -> source` interface.

## Constraints (AGENTS.md, enforced by the user)

- **Never edit vendored trees in place.** Patches under
  `third_party/patches/<project>/` are the source of truth; `apply_patches.py`
  and `scripts/check_pins.py` must pass.
- **No arbitrary or hardcoded limits.** The stack max is declared or the FreeBSD
  default; nothing hardcodes a small one.
- **No seL4 errors or warnings** in the log. Every `<<seL4 …>>` line and every
  `command faulted` is a hard failure — read it, do not wave it off. (This arc's
  whole confusion began by reading the `'file not found'` text and stepping over
  the fault.)
- **No flakes.** A broken run is broken; find out why.
- `make`/`make run` under `timeout`; afterwards `ps -eo args | grep '[q]emu-system'`
  must be empty.
- **STOP and ask for clarification**; never unilaterally change the plan.
- Run logs contain ANSI colour and NUL bytes — use `grep -a`, and strip escapes
  before reading.

## Watch out for

- **LLVM rebuilds are expensive.** `scripts/build_llvm.sh` skips if
  `out/runtime/llvm-install` exists; `rm -rf out/runtime/llvm-install` to force
  one after a vendored-tree patch lands, or the run uses stale libraries.
- **The layout addresses are load-time.** `_end`, the block, the IPC and the
  stack are computed by the spawner, so the measured numbers (`_end 0x480c818`,
  stack `[0x480f000,0x481f000)`) move; use `llvm-addr2line`/`llvm-readelf` on the
  built binary, not the transcript's numbers.
- **The diagnostic baseline is 0 probe lines.** The probes above are temporary;
  the guest's log should carry none of them when this task lands.
- **The pager's `alloc` is synchronous in the fault path.** Safe (memory does not
  depend on the pager) but record it — the boot deadlocked once on "a granter
  must not wait behind what it is granting" (`specs/memory.md`).

## Read first

- `specs/pager.md` — the decision, the measurement, the plan (this task).
- `specs/memory.md` — the memory service: how grants are badged to a child and
  reaped by `release`, why a pager owns no memory of its own.
- `specs/launch.md` — the spawn request, the fault supervisor, the stack ask.
- `specs/userland.md` — "a service's stack is what the spawner gives it".
- `AGENTS.md`.

## Related, not this task

Once the pager is in, the compiler-with-headers WIP above should go green, and
`Sys:Development/Include` becomes the sysroot the follow-on work (`specs/
development.md`) stages and searches. The stack's *max* declaration and the
`Stack`-style launch override are step 2; the note for per-binary initial/grow is
not needed until something wants it.
