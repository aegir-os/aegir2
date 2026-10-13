# The POSIX conformance suite

Status: in progress. This spec fixes what "conformance" means for Aegir's POSIX
layer, so that **completeness is a read-out and not a claim**. `specs/posix.md`
fixes the surface a POSIX program sees; this fixes how that surface is proved,
and how a hole in it is found.

## The problem this exists to fix

A conformance suite that covers whatever its author happened to think of can be
reported as complete while covering four calls: the first cut of
`aegir-posix-conformance` covered `mmap`/`munmap`/`mprotect`/`posix_madvise`,
and it was described as complete. Nothing in the tree said which interfaces were
in the surface, so there was no list to check against and prose stood in for it.

The lesson is not "be more careful" -- a reminder loses to a mechanism. The
lesson is that the surface has to be **written down**, and coverage has to be a
check that can fail.

## The surface

`manifests/posix-surface.toml` enumerates the surface, organised by area. Each
area is either `todo` (not yet enumerated -- the check fails) or `covered`. Each
interface is one row:

```toml
[[interface]]
name = "mmap"
area = "memory"
state = "implemented"   # or "unimplemented"
syscall = 222           # the riscv64 number the call reaches, or 0 for libc-only
clause = "mmap.html"    # the Issue 8 page its cases are derived from
```

- **`implemented`** -- Aegir answers the syscall, or the call is libc-only and
  the stand-up provides it. The suite must have at least one case derived from
  the standard.
- **`unimplemented`** -- Aegir does not answer the call. It must return the
  standard's `ENOSYS` ("Function not implemented"), which is exactly what an
  unanswered syscall returns (`libs/hosted/aegir-heap/src/heap.cc:1868`), and the
  suite must assert it. A hole is a row, not an absence.

The truth for "does Aegir answer this syscall" is the dispatcher's switch
(`heap.cc`'s `vsyscall`), never the manifest: the two are cross-read, and either
disagreeing with the other is a failure.

## The check

`make check-posix-coverage` runs `scripts/check_posix_coverage.py`, which reads
three things:

1. the dispatcher's answered syscalls, parsed from `heap.cc`'s switch;
2. `manifests/posix-surface.toml`;
3. the suite's cases, parsed from `apps/hosted/aegir-posix-conformance/src/`
   (`check("name", ...)` and `unavailable("name", ...)`).

and fails, **naming the interface**, when

- an area is `todo`;
- an `implemented` interface's syscall is not answered by the dispatcher;
- an `unimplemented` interface's syscall *is* answered (a stale hole);
- an interface has no case in the suite;
- a case names an interface that is not in the manifest.

So `make check-posix-coverage` **is** the worklist: while the suite is
incomplete it prints the interfaces and areas still to do and exits non-zero.

## The suite

`apps/hosted/aegir-posix-conformance` is a boot service, packed into director's
initrd and started from the reduced `posix-test` manifest. `make posix-test`
runs it and `make check-posix-coverage` gates it. Nothing is typed: there is no
shell, no greeter and no login.

It prints one line per case -- `posix: ok|FAIL <area>: <interface>: <test>` for
an implemented interface, `posix: unavailable <area>: <interface>` for a hole --
and one marker, with **three** outcomes rather than two:

```
AEGIR_POSIX_CONFORMANCE_CONFORMS <passed>/<total> (<skipped> skipped, 0 holes, 0 deviations)
AEGIR_POSIX_CONFORMANCE_GAPS     <passed>/<total> (<skipped> skipped, <holes> holes, <deviations> deviations)
AEGIR_POSIX_CONFORMANCE_FAIL     <passed>/<total> (<skipped> skipped, <holes> holes, <deviations> deviations)
```

**`GAPS` is not a pass.** "The suite ran to its end" and "Aegir conforms" are two
claims, and only the second is a pass: a run that recorded holes and deviations
prints `GAPS`, and the runner reports it as *ran, with gaps recorded* -- never as
`PASS`. `CONFORMS` needs both counts to be zero; `FAIL` means a case failed
outright. The runner stops on any of the three (`Target.marker`,
`gaps_marker`, `failure_marker`).

## What a case is

A case comes from the standard, never from Aegir's implementation: it names the
Issue 8 page and clause it is derived from (`third_party/posix-spec/issue8`,
fetched by `make deps` and read, never vendored). A case whose subject Aegir
does not implement is written first, measured failing, and the fix -- in the
dispatcher or in the personality -- lands with it, so the case verifies
something. A call outside the profile is `unavailable`, named, not silent.

Coverage is a read-out, not a target: the number the marker prints is the
count of cases that ran, and the count of interfaces the check found is the
measure of the surface.
