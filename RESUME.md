# Task: file-backed paging — a clean baseline, an unsolved wedge

Repo: `/home/rroland/src/aegir2`. **`main` is green** (`e5b9502a`: the
per-thread receive-slot fix; `make run TARGET=aegir-8g-smp4` passes with
`CXX_SMOKE_OK`, `AEGIR_HELLO_OK`, `PASS aegir-8g-smp4 booted
(AEGIR_BOOT_OK)`). **File-backed paging is not landed.** A clean
implementation exists on `wip/fb-clean` (`6e73adc8`), it fills every page
correctly, and it still wedges `type Sys:S/Shell-Startup | aegir-read`. This
session ended at a dead end on that wedge; this file is the handoff, and it
records the ruled-out list so the next session does not re-litigate it.

## Where the wedge stands (measured, on the clean tree)

- `launcher: command started type` and `launcher: command started aegir-read`,
  then **silence**. The guest is **busy** (it survives the runner's 30 s idle
  check and hits the 300 s cue timeout). There are **no faults**: the bounded
  `20000 faults` counter in `grow_stack` never fires.
- The log is clean — about **2 300 lines**; with the diagnostics that this
  session added it was **81 363**. Read that difference first (see "the wrong
  turns").
- On the *instrumented* tree (superseded, but a lead) the chain was:
  `type`'s `openat("Sys:S/Shell-Startup")` → `enter` / `slot` →
  `resolve: call Sys:S/Shell-Startup` → the VFS's `ar: in Sys:` → **no
  `ar: kind`**. Those runs were flooded by per-op prints, so treat that chain
  as a lead, not proof.
- A paused vCPU sample showed the running thread was a **file-backed command**
  (`r--` / `r-x` pages under the W^X mapping) executing its **own code** — not
  a wild jump.

## What is landed and verified

- **The per-thread cap-receive slot** (`4a22f3e9`, `e5b9502a`, on `main`). The
  launcher's fault supervisor is a **second thread sharing the process CSpace**
  (`main.cc`: "Its own CSpace and VSpace are this process's"), but the IPC layer
  had **one** `kSlotReceiveCap` (=5), deleted by `set_receive_path` before
  every receive and every call. The supervisor's fault-time `call_transfer`
  (`pager_chunk`'s `mem.main`, and `read_frame` per faulted page) therefore
  deleted a capability the serve loop had just received and had not yet moved
  (`handle_spawn` takes it only after locking `g_service_mutex`, which the
  supervisor holds across the whole `grow_stack`). Fixed by
  `aegir-bootstrap`'s per-thread `receive_slot` / `set_receive_slot` (a
  `thread_local`, `0` = the default), `aegir-ipc` using it, and
  `aegir-thread::Builder` allocating one plain slot per thread and starting it
  at a **trampoline** that sets it in the new thread's own TLS. The trampoline
  is required: writing the slot from the builder measurably broke the eager
  acceptance (stopped at `wait`, guest idle) because a hosted boot thread and a
  sel4runtime-started thread do not share a TLS layout. The director's
  hand-started supervisor gets one too.
- **The file-backed implementation** (`wip/fb-clean`, 7 files, 194 lines,
  no diagnostics):
  - `request.lazy_image` + `segments_out` (`spawn/process.h`, `process.cc`):
    the spawner **records** each `PT_LOAD` (vaddr, memsz, offset, filesz,
    flags) and **maps nothing**.
  - `spawn::Region`'s file arm (`spawn/pager.h`, `pager.cc`): a fault in a
    segment page carves a frame, `PagerSource::fill`s it, and
    `map_with_tables` maps it with the segment's rights and attributes.
  - `ServiceKit::watch_stack` records `file_regions` per command and retains
    `image_fd`; `grow_stack` checks the stack region first, then the file
    regions; `pager_fill` (`service_launch.cc`) **zeroes the frame** (the gap
    before `vaddr` and the `.bss` past `filesz`) and reads the page with
    `read_frame`, falling back to `read_at`.
  - Pages carry the **segment's rights** (`Region.writable` / `executable`
    from `Segment.flags`): `seL4_AllRights` for `PF_W`, `seL4_CanRead`
    otherwise, and data pages are `seL4_RISCV_ExecuteNever` (W^X). This matches
    the eager spawner.
- The fills are **correct** — while the verify was in place every page matched
  the file byte-for-byte, including partly file-backed pages (`frame_offset !=
  0`) and `.bss` pages.

## Ruled out — with the evidence, do not re-litigate

| hypothesis | how it was killed |
|---|---|
| the shared receive slot | real, and **fixed** on `main`; the file-backed wedge remains |
| a wild jump into rodata | W^X (data execute-never) produced **no** fault; the process was *reading* rodata |
| the VFS flooded by `aegir-read`'s poll | the fd's reads are **direct to the pipe**: `resolve_target` returns the pipe's own cap, and `union_open` bypasses a read open (`reply_cap(out, …, badged_member(member, badge))`). The VFS is not in the read path |
| a fault storm | the fault-reply counter showed `gui-demo` (pid `…024`, image `0x10000..0x12a470`) walking sequential pages `0x10e018 … 0x121028`, then stopping thousands of lines before the wedge |
| the used-slot wall | no heap refusal marker fired (`page-no-slot`, `page-no-piece`, `page-map-refused`, `chunk-refused`, `slot-refused`); `sys_mmap` fine |
| a fault loop / re-fault | the bounded `20000 faults` counter in `grow_stack` never fires on the clean tree |
| CPU starvation by the reader's poll | `seL4_Yield()` between polls on the **clean** tree changes nothing |
| the diagnostics | the clean tree wedges identically without them |

## The wrong turns (so they are not repeated)

- **Every `debug_write` is a synchronous `seL4_Call` to the single-threaded
  logger** — `aegir-runtime/src/debug.cc`'s `console_send` does
  `seL4_Call(port, info)` unless the process *is* the logger. A per-page or
  per-op marker is therefore an IPC per page/op. `3dd6890b` shipped
  `spawn: pager fill …` / `fill verify …` **per page** and
  `spawn: file region fault/handled` **per fault**; this session added ~40
  more per-op markers. The runs grew to 80 000+ lines, got slower, and the
  **last line was a print that never lands** — the logger was wedged, not the
  resolve. `AGENTS.md` names this twice ("a diagnostic that changes what it
  measures"; "a probe must be bounded"). **A probe is an IPC: bound it, and
  prefer a counter that prints once over a marker that prints per operation.**
- Two causes were asserted and retracted: "the single-threaded VFS is flooded
  by the poller" and "a wild jump into rodata". Both would have been avoided by
  reading one layer further (the fd's read path; the segment flags) before
  proposing a fix. That is the `AGENTS.md` rule: read the established
  implementation before designing.
- The QMP vCPU sampler was only ever run on **flooded** runs, so its samples
  were confounded by the flood.

## Next measurement (do this first)

**Run the QMP vCPU sampler on the clean tree** (`wip/fb-clean`). With no flood
it can name the one spinning thread and its PC, which is the question this
session could not answer by reading.

The technique: run the runner in the background; poll the console log; when it
stops growing for ~20 s, connect to `out/<target>/qmp.sock` (the runner uses
one-shot QMP connections and is not connected while it waits), send
`{"execute":"stop"}`, then `human-monitor-command` with `info registers`,
`x/24i <pc>`, `x/64gx <sp>` and `info mem`, and match the mapped extent to the
spawn log (`/tmp/cap5.py` from this session did exactly that; it is temporary).
Useful constants: `nmspace::kMethodResolve = 2`,
`volume::kMethodReadFrame = 25`.

If the sampler still cannot name it, the next bounded probe is a **counter**
(prints once at a threshold, not per call) on the client side of
`Consumer::call_transfer` and in the VFS's `answer_resolve`.

## The independent item: the pipe's POSIX read

`aegir-fs-pipe`'s `answer_read` / `answer_read_handle` are **non-blocking**:
an empty pipe replies `got=0`, so `aegir-read` polls, one round trip per poll.
POSIX says an empty pipe with the writer open **blocks** (`heap.cc`'s
`sys_read`: only the console stream "waits inside the call").

The fd is **already direct to the pipe** (see the table), so this is a
**pipe-only** change: on an empty read with the writer open, hold the caller's
reply with `aegir::signal::Reply_holder` (`save()` / `reply()`;
`console_stream` is the precedent, `signal.h` is the class), and answer it when
data arrives or the writer closes. It needs one CSpace slot per pending read —
the pipe has no slot allocator; replicate the launcher's `first_free`
computation (`kSlotFirstDeclared` bumped past the block's declared entries).

This is a correctness fix and removes a busy loop. It is **not** the
file-backed wedge (ruled out above) — do not expect it to fix it.

## Branch state

| branch | commit | what |
|---|---|---|
| `main` | `e5b9502a` | green: the per-thread receive-slot fix |
| `wip/fb-clean` | `6e73adc8` | the clean file-backed pager (7 files, 194 lines, one bounded fault counter). **Wedges.** |
| `wip/file-backed` | `55ad58d9` | superseded: the original file-backed WIP with per-page probes |
| `wip/fb-fixed` | `2063e320` | superseded: file-backed + the full probe set |
| `wip/compiler-sysroot` | `1bd5deac` | merged into `main` |

## Still open (from before, unchanged)

1. **The used-slot wall** (`specs/userland.md`, "a used slot is still
   allocation-only"): a process that maps and unmaps still fills its CNode; a
   release gives memory back and not slots. The musl patch sidesteps it for
   thread stacks; a growable CSpace for boot services is the general fix. Not
   this wedge (no refusal fired), but real.
2. **The root's two-level CSpace** (optional, uniformity): director holds
   22368 caps, so `l2 >= 15` and per-service CNodes would be ~512 KB each. The
   pager has its own pool; decide this on its own merits.
3. **`check-bfs` as a prerequisite of `build`/`run`** (the multi-leaf directory
   case below), the way `check-allocator` is.

## Carried findings (landed; kept because they bite if forgotten)

- `mkfs_bfs._tree`'s internal-node offsets were off by one (`NODE * (2 +
  len(nodes) + n)`; a node at list index `i` is written at `NODE * (i + 1)`).
  A directory that split into two leaves had `root == maximum_size`. That is
  what finished clang. **A host case for a multi-leaf directory is still
  missing** — item 3 above.
- musl reads `PT_GNU_STACK.p_memsz` for `__default_stacksize`
  (`projects/musl/src/env/__init_tls.c:98`). A tracked patch
  (`third_party/patches/projects/musl/0004-…`) keeps it at
  `DEFAULT_STACK_SIZE`; the flag is only the pager's declaration.

## Read first

- `specs/pager.md` — the decision, the resolution order (launch request >
  `stack_kib` > `PT_GNU_STACK.p_memsz` > default 16 pages / 8 MiB / 128 KiB
  step), and what landed.
- `apps/freestanding/aegir-fs-bfs/src/volume.cc` — `read_block`, `read_stream`,
  `open`.
- `apps/freestanding/aegir-fs-bfs/src/main.cc` — `answer_read_frame`,
  `answer_read_handle`, `answer_stat`.
- `apps/freestanding/aegir-vfs/src/main.cc` — `answer_resolve`,
  `resolve_path`, `union_read`, `union_open`.
- `libs/freestanding/aegir-vfs-client/src/vfs.cc` — `Namespace::resolve`,
  `Volume::stat`, `open_read` (the bypass path).
- `libs/aegir-posix/src/files.cc` — `openat` and its stages.
- `libs/freestanding/aegir-runtime/src/debug.cc` — `console_send`: **every
  print is an IPC**.
- `libs/freestanding/aegir-signal/include/aegir/signal.h` — `Reply_holder`.
- `apps/freestanding/aegir-fs-pipe/src/main.cc` — `answer_read`, and the
  non-blocking contract in its header.
- `libs/hosted/aegir-spawn-service/src/service_kit.cc` / `service_launch.cc` —
  `watch_stack`, `grow_stack`, `pager_carve`, `pager_fill`.
- `specs/memory.md`, `specs/launch.md`, `specs/userland.md`,
  `third_party/patches/README.md`, `AGENTS.md`.

## Constraints (unchanged)

- Builds/runs under `timeout`; check for orphaned `qemu-system` afterward.
- No `<<seL4 …>>` line, no `command faulted` on a green run.
- Never edit a vendored tree in place; patches under `third_party/patches/` are
  the source of truth, and `make deps-check` must pass.
- A check that cannot fail the command is not a check.
- STOP and ask rather than changing the plan.
- **A probe is an IPC.** Bound it; prefer a counter that prints once. Never add
  a per-page or per-op `debug_write` to the pager or a service: it saturates
  the single-threaded logger and every observation after it is about the
  logger, not the bug.
