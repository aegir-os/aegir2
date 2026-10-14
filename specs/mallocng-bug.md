# mallocng faults over `aegir-heap`

**Status: open.** The hosted tier builds musl's older allocator, `oldmalloc`,
which does not fault (`specs/cxx.md`). This records the fault so the fix can be
made in the heap, where it belongs, rather than in musl.

## Symptom

The on-device acceptance's link step faults. `ld.lld` is spawned, reads the
staged sysroot, and dies in the middle of parsing its inputs:

```
launcher: command faulted, fault 5, pid 4611686018428502070 (Sys:Development/C/ld.lld)
```

The fault is a load page fault (`scause = 0xd`) taken at `ld s7, 0x10(s7)` with
`s7 = 0`, inside musl's `__libc_realloc`, reached from
`SmallVectorBase<unsigned int>::grow_pod` -> `llvm::safe_realloc`. Before the
launcher's fault path landed (`specs/services.md`) the thread simply sat
`BlockedOnSend` -- there was no fault receiver -- so the session wedged with no
line saying why.

## Reproduction

Build musl with its vendored default and run the acceptance:

```sh
rm -rf out/runtime/musl-install out/runtime/musl-build
AEGIR_MUSL_MALLOC=mallocng scripts/build_musl.sh aegir-8g-smp4
make build TARGET=aegir-8g-smp4
make run   TARGET=aegir-8g-smp4
```

The fault follows the allocator, not the input: `AEGIR_MUSL_MALLOC=oldmalloc`
(now the default, `scripts/build_musl.sh`) removes it and the linker runs on to
the *next*, unrelated error -- `SCRATCH:hello.o: cannot link object files with
different floating-point ABI from .../crt0.o` (the `aegir-clang` ABI default,
tracked separately). Nothing but the malloc backend changes between the two
runs: same kernel, same heap, same lld, same inputs.

## What the fault is

A temporary probe in `llvm/lib/Support/SmallVector.cpp`'s `grow_pod`
(`third_party/patches/projects/llvm-project/0003-smallvector-grow-probe.patch`)
prints every sizeable heap-storage growth. On the faulting run there are eight,
and seven are ordinary -- a full vector growing, `size == cap`:

```
AEGIR-SVV grow caller=0x27f7964 this=0x102303330 beginx=0xff112000 firstel=0x102303340 size=4095 cap=4095 TSize=8 MinSize=4096 bytes=65528
```

The eighth is impossible for a live `SmallVector`:

```
AEGIR-SVV grow caller=0x275b7c0 this=0xff442040 beginx=0xff43d000 firstel=0xff442050 size=8192 cap=0 TSize=8 MinSize=8193 bytes=65544
```

`size > capacity`, and out-of-line storage (`BeginX != FirstEl`) with
`capacity == 0`. `safe_realloc(BeginX, 65544)` then reads the group header at
`BeginX - 16`, gets 0, and faults. The caller symbolizes to
`lld::elf::ObjFile<...>::createInputSection` (entry + 4, i.e. a corrupted or
mis-attributed frame; the function grows no vector directly -- the growth is in
an inlined section constructor).

So the object at `this` is inconsistent: `BeginX` and `Size` were written but
`Capacity` was not, on heap that `aegir-heap` zeroes. That is the shape of a
`SmallVector` whose `Capacity` word was never written, or of storage reused and
only partly overwritten.

## What is ruled out

Measured, not argued:

- **lld's own logic.** Built host lld from the *pinned* revision
  (`llvmorg-20.1.8`, `87f0227cb6`) with `-DLLVM_USE_SANITIZER=Address`, x86-64,
  and linked the same staged sysroot. It parsed every input -- all 1580 archive
  members -- with every growth valid (`size == cap`, through `8191`) and **no
  ASAN report**. So the same source parses the same inputs without faulting on
  an ordinary allocator, and the fault is not in what `createInputSection`
  does.
- **Our `mmap` reuse leaking zeroed memory.** The heap's `mmap-reused` marker
  is 0 over the whole run, and every refusal marker (`page-slot-refused`,
  `page-no-slot`, `page-no-piece`, `page-kernel-error`, `mmap-window-refused`,
  `chunk-refused`, `slot-refused`) is 0 too.
- **A freed group.** mallocng's `free_group` sets a freed nested group's
  header to 0 (`src/malloc/mallocng/free.c`), which would explain a 0 at
  `BeginX - 16` -- but with the probe in place that branch never runs
  (`AEGIR-FREE` count 0), so the 0 is not its marker.
- **`mmap`/`munmap`, the `mremap` stub, and region arithmetic.** `BeginX` is in
  no `munmap` range; the `mremap` stub is correct; `mmap`/`munmap` release
  *pairs* were already checked over a full boot and are well formed
  (`specs/memory.md`).

## Where to look next

The fault tracks mallocng's *interface* to the heap, so the suspects are the
syscalls mallocng uses that `oldmalloc` does not:

- **`madvise(MADV_FREE)`.** `mallocng`'s `free` advises whole pages within a
  freed slot (`src/malloc/mallocng/free.c`). `MADV_FREE` is a Linux value (8),
  outside the POSIX range our `sys_madvise` accepts, so it returns `-EINVAL`
  (`libs/hosted/aegir-heap/src/heap.cc`). It is a hint, which mallocng ignores,
  so this is not the fault on its own -- but it is a real spec gap, and any
  caller that *does* honour the return would be surprised.
- **`mmap` for metas and group layout.** `mallocng` maps metas and lays out
  groups with a `struct meta`/`struct group` header that a `SmallVector`'s
  object lands inside. Its alignment (`libc.page_size`) and zero-fill
  expectations (`specs/cxx.md`) are the contract to check.
- **`donate`.** `mallocng`'s `donate.c` zeroes a range and carves metas out of
  it (`m->mem->meta = m`), unlike anything `oldmalloc` does.

The reproduced failing run's log and the probe output are the starting point:
the bad growth names `this = 0xff442040` and `BeginX = 0xff43d000`, so the next
probe is the allocation history of those two addresses -- which mallocng meta
or group each came from, and whether either was ever freed and handed back.
