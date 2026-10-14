# Task: finish the acceptance — two remaining bugs (Aegir, /home/rroland/src/aegir2)

## Where the tree is

Run `make build TARGET=aegir-8g-smp4 && make run TARGET=aegir-8g-smp4`. The
acceptance **boots end to end** now: it reaches `AEGIR-ACCEPTANCE-LINKED` and
`AEGIR_HELLO_OK` is printed, `23 ready, 0 faulted`. Two bugs remain, both
described below. Everything landed so far is uncommitted diagnostic work plus
two real fixes (see "Already landed").

The signatures to reproduce:

```
$ make run TARGET=aegir-8g-smp4
...
    <<seL4(CPU 0) [decodeInvocation/643 ... "rootserver" @...]: Attempted to invoke a null cap #N.>>   (188 of these)
    ld.lld: error: SCRATCH:hello.o: cannot link object files with different floating-point ABI from /Sys/Development/Libs/crt0.o
```

## Already landed (do not redo)

1. **The hosted tier builds `oldmalloc`, not `mallocng`.**
   `scripts/build_musl.sh` defaults `MALLOC_IMPL="${AEGIR_MUSL_MALLOC:-oldmalloc}"`
   (`--with-malloc=`). The reason is `specs/mallocng-bug.md`: mallocng faults
   over `aegir-heap` (a `SmallVector` grown with `size=8192, cap=0`). That is
   documented and parked. `AEGIR_MUSL_MALLOC=mallocng` rebuilds the vendored
   default to reproduce it. `specs/cxx.md` records the reversal.

2. **The scratch window no longer trusts a computed span.**
   `libs/freestanding/aegir-mem/src/vspace.cc`:
   - `Scratch::map` treats the kernel's `seL4_DeleteFirst` ("already mapped") as
     the occupancy test and **skips** the page (`kMaxWindowSkip` bound) instead
     of failing. This is `libsel4utils`' rule ("nothing is ever mapped into a
     non-EMPTY entry", `projects/seL4_libs/libsel4utils/src/vspace/bootstrap.c`)
     reached without a page-table mirror: the kernel already knows.
   - `limit_ = base_ + kLargePage`, not `round_up_to_2MiB(base_)`. The old
     rounding made the initial window `2 MiB - (base_ mod 2 MiB)` — anywhere from
     one page to 2 MiB — so the initrd (which moves `base_`) could shrink the
     window to a single page. This was the whole "layout-sensitive" saga.

   The remaining, optional half of this (not done): seed a real mirror from the
   three sources `reserve_initial_task_regions` uses — `[__executable_start,
   _end)`, the IPC buffer page, and the bootinfo + `extraBIPages` — and place the
   window only in unmarked space, so it never even *attempts* a reserved page.
   Today `base_ = max(_end, ipc_end, bootinfo_end)` already sits above all three,
   so this is belt-and-suspenders rather than a fix; the skip is what makes the
   window survivable.

## Bug 1 — the console endpoint cap is emptied (188 seL4 errors)

**Symptom.** 188 `seL4` kernel lines, all `"rootserver"`, all
`decodeInvocation: Attempted to invoke a null cap #N`, where `N` is a *low* slot
(this run: a handful of distinct values in the 900s). The faulting user PC
symbolizes to a **console write**:

```
aegir::nmspace::pack_string(...)                 nmspace/.../nmspace.h
aegir::console::stream_write(...)                console/console_stream_client.h
aegir::heap::trace_phase(char const*)            libs/hosted/aegir-heap/src/heap.cc
```

So the heap is trying to `debug_write` (its own trace output) and the **console
stream endpoint capability is empty**. Something deleted or emptied a slot the
process still holds.

**What is ruled out.** It is *not* `Scratch`/`free_slot`: a window-slot ownership
check (`slot_is_ours`) was tried and fired **0 times** (and regressed boot, and
was reverted). The frame bookkeeping's low-slot trace (`heap: cap-record` /
`cap-delete` in `libs/hosted/aegir-heap/src/heap.cc`) fired on low slots 39–112
as `cap-record` but never as `cap-delete`, so `sys_munmap` is not deleting the
console cap either — at least not in the runs measured.

**Next step.** Name the slot and the deleter. The two candidates are
`seL4_CNode_Delete` in `heap.cc` (three sites: 694/703 are the scratch receive
slot, 1288 is `sys_munmap`'s frame) and any `free_slot`/`slot_failed` path in
`aegir-mem`. Add a trace at each `CNode_Delete` naming the slot, run until the
null cap's number appears, and the deleter names itself. `heap.cc` already has
the `debug_write` facility; `aegir-mem` now links `aegir-runtime` (see below).

## Bug 2 — the on-device link fails on the floating-point ABI

**Symptom.**

```
ld.lld: error: SCRATCH:hello.o: cannot link object files with different
floating-point ABI from /Sys/Development/Libs/crt0.o
```

The guest's `aegir-clang` (`apps/hosted/aegir-clang`) compiles
`Sys:Development/hello.c` to `SCRATCH:hello.o` with soft-float (`lp64`), while
the staged `crt0.o` (from `aegir-crt0`) is hard-float (`lp64d`, Aegir's pinned
ABI — `specs/build.md` says rv64imafdc/lp64d).

**Confirmed by reproduction.** On the host, `clang --target=riscv64-unknown-elf
-c hello.c` (no `-mabi`) produces the same mismatch, and adding `-mabi=lp64d`
makes it link. So the fix is that the **driver default must be the Aegir ABI**
(`-march=rv64imafdc_zicsr_zifencei -mabi=lp64d`), the way the build's own
`CFLAGS` set it.

The acceptance's compile step is in `scripts/targets.py:2010`:
`cc -v -ccc-install-dir Sys:Development/C --target=riscv64-unknown-elf -c
Sys:Development/hello.c -o SCRATCH:hello.o` — the driver is `aegir-clang`,
deployed as `cc`/`clang`. Either the driver must inject the ABI, or the
acceptance's compile line must carry it; prefer the driver (the spec says the
device compile and the build's compile must agree).

**Note.** This error is only visible now because it used to be masked: with
mallocng, `ld.lld` faulted during parsing before it ever reached the ABI check.

## Constraints (from AGENTS.md and this arc)

- **Never patch the seL4 kernel.** The elfloader (`tools/seL4/elfloader-tool`)
  is the sanctioned patch point (`third_party/patches/tools/seL4/`).
- **Patches under `third_party/patches/<project>/` are the source of truth**;
  never edit vendored trees in place.
- **No arbitrary or hardcoded limits; capacity grows on demand.**
- **No seL4 errors or warnings** — 188 is a failure, not a warning.
- **No flakes:** a broken run is a broken run. Runs are deterministic per build
  (three identical runs gave 204/204/204 earlier), but *any* change to a binary's
  size can move an address and flip an outcome — argue from invariants, not from
  one green run.
- Long runs use `timeout`; afterwards `ps -eo args | grep '[q]emu-system'` must
  be empty.

## Diagnostics in the tree (strip when both bugs are fixed)

These are all "AEGIR PROBE"/"TEMP" instrumentation, deliberately left in:

- `libs/freestanding/aegir-mem/src/allocator.cc` + `include/aegir/mem/allocator.h`:
  `cap_live`, `slot_was_freed`, `retype_source_live` guards on the four node-cap
  retype sites (`refill_inner`, `alloc_object`, `alloc_pages_run`,
  `alloc_cnode_at_l1`). These cut seL4 errors **5617 → 204**; they *refuse* a
  retype from a freed node cap instead of invoking the kernel. The root cause
  (a live piece whose cap was already given back) is **not** fixed.
- `libs/freestanding/aegir-mem/src/vspace.cc`: `window: init`, `map ok`,
  `map_at`, `map_large`, `skip occupied`, `map failed` prints.
- `libs/freestanding/aegir-spawn/src/process.cc`: `spawn: block_at` /
  `block failed` prints.
- `libs/hosted/aegir-heap/src/heap.cc`: `cap-record`/`cap-delete` prints.
- `third_party/patches/projects/llvm-project/0003-smallvector-grow-probe.patch`:
  the `AEGIR-SVV grow` probe in `SmallVector::grow_pod` (referenced by
  `specs/mallocng-bug.md`).
- The `aegir-runtime` link added to `libs/freestanding/aegir-mem/CMakeLists.txt`
  and `libs/freestanding/aegir-spawn/CMakeLists.txt` is only there so those
  libraries can `debug_write`; drop it with the prints.

## Orientation

- Build/run: `make build TARGET=aegir-8g-smp4` / `make run TARGET=aegir-8g-smp4`
  (`BUILD_TIMEOUT=1800`, `RUN_TIMEOUT=900`). Run logs contain NUL bytes — use
  `grep -a`.
- The acceptance's link command is generated in `scripts/run_target.py:821`
  (`SUBSTITUTIONS["link"]`), staged under `out/aegir-8g-smp4/development/Libs/`.
- Specs: `specs/cxx.md` (the hosted C/C++ runtime and allocator choice),
  `specs/mallocng-bug.md` (the parked mallocng fault),
  `specs/userland.md` (why not `libsel4utils`), `specs/memory.md` (heap).
