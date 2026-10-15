# Task: give Aegir its own triple — `riscv64-unknown-aegir-musl`

Repo: `/home/rroland/src/aegir2` (branch `main`). Make `riscv64-unknown-aegir-musl`
the **one** triple for the whole system: the compiler's *output* triple and the
*input* triple every part of the hosted tier is built for. The decision and its
reasoning are already written down (`specs/clang-on-aegir.md`, "The target
triple" and "The hosted tier's input triple"); **none of it is implemented yet.**

## Where the tree is

```
make build TARGET=aegir-8g-smp4     # BUILD_TIMEOUT=1800
make run   TARGET=aegir-8g-smp4     # RUN_TIMEOUT=900
```

The acceptance is **green** on `main`: `PASS aegir-8g-smp4 booted
(AEGIR_BOOT_OK)`, `23 ready, 0 faulted`, **0 seL4 errors**, 0 test failures,
clean log, no leftover QEMU. `git status` is clean; the last two commits are the
triple decision (`1d212221`) and the hosted-tier plan (`e2b087ed`). Keep it green
at every step.

## The decision (settled — do not relitigate)

`riscv64-unknown-aegir-musl` — Linux's shape with `linux`→`aegir`:

| slot | value | why |
|---|---|---|
| arch | `riscv64` | |
| vendor | `unknown` | Linux uses it too; the slot names a *platform* vendor and Aegir has none |
| os | `aegir` | the OS slot is what `isOSBinFormatELF`, the default ABI and the ToolChain key on |
| environment | `musl` | the libc a hosted program links; no non-test `isMusl()` use fires for a non-Linux OS |

`-elf` is **not** a field to add — it is the *object format*, already ELF for
riscv64. Putting `aegir` in the **vendor** slot (`riscv64-aegir-…`) is strictly
worse: LLVM then inserts `unknown` for the OS, which is the slot that matters.

## Current state: three values for one system

| what | triple | where |
|---|---|---|
| the whole Aegir build | `riscv64-unknown-elf` | `configs/riscv64-qemu-virt.cmake`'s `TRIPLE` |
| musl / libc++ / compiler-rt | `riscv64-unknown-elf` | `scripts/build_musl.sh`, `build_libcxx.sh`, `build_compiler_rt.sh` |
| LLVM's Support only | `riscv64-unknown-linux-musl` | `scripts/build_llvm.sh`'s `BUILD_TRIPLE` |
| the guest's compile | `riscv64-unknown-elf` | `scripts/targets.py` acceptance line |
| **target (this task)** | `riscv64-unknown-aegir-musl` | everywhere |

Two values for one system is the hazard: `__linux__` is on for LLVM's translation
units and off for the apps', so libc++'s inline header code can differ between
them. `build_llvm.sh`'s Linux-musl is a **labelled crutch**, not a claim that
Aegir is Linux — it describes the syscall ABI musl's port already speaks.

## Why this is small (the measurement is already done)

Fuchsia — a real, non-Unix OS that LLVM supports — is the control:

- `llvm/lib/Support/Unix`: **3 files** (`Memory.inc`, `Path.inc`, `Program.inc`)
- `Triple::Fuchsia`: **5 files** (`llvm/include/llvm/TargetParser/Triple.h`,
  `llvm/lib/TargetParser/Triple.cpp`, `clang/lib/Basic/Targets.cpp`,
  `clang/lib/Driver/Driver.cpp`, `clang/lib/Lex/InitHeaderSearch.cpp`)
- `libcxx/include`: 4 files; `libcxx/src`: 2 files

Total **~30 references across ~15 files**. Aegir's is smaller:

- **musl has zero `__linux__`** — its port is the three tracked patches routing
  every syscall through `__sysinfo` (`projects/musl/arch/riscv64/syscall_arch.h`).
- `0005-getMainExecutable-argv0.patch` already fixes `Unix/Path.inc`'s one `#error`.
- `LLVM_ENABLE_THREADS=OFF` keeps `Unix/Threading.inc`'s nine branches out.
- The remaining hard `#error` (`Unix/Process.inc`'s page size) is a **configure**
  value (`HAVE_GETPAGESIZE`), not an OS branch.

## The plan — land each step as its own commit, re-run green each time

1. **The OS enum.** `OSType::Aegir` in `llvm/include/llvm/TargetParser/Triple.h`
   plus its `parseOS`/`getOSTypeName` cases in `llvm/lib/TargetParser/Triple.cpp`
   (beside `Linux`, `Serenity`, `HermitCore`). As a new tracked patch
   `third_party/patches/projects/llvm-project/0006-riscv64-aegir-triple.patch`
   (apply it, then `make deps-check` / `scripts/check_pins.py` must pass).
2. **The ToolChain.** `case Triple::Aegir` in `clang/lib/Driver/Driver.cpp`, with
   the TargetInfo and the **default include search** beside it
   (`clang/lib/Basic/Targets.cpp`, `clang/lib/Lex/InitHeaderSearch.cpp`) — the
   latter is where `specs/development.md`'s `Sys:Development/Include` default
   belongs. Today `aegir` is `UnknownOS` and reaches `toolchains::Generic_ELF`.
3. **The Unix layer.** The `Unix/Process.inc` page-size `#error` (answer it with
   `HAVE_GETPAGESIZE` in the configure), and any `Unix/*.inc` branch whose generic
   `#else` path is actually wrong for Aegir. Expect this to be a handful.
4. **The build triples.** `configs/riscv64-qemu-virt.cmake`'s `TRIPLE` and the
   `riscv64-unknown-elf` in `build_musl.sh` / `build_libcxx.sh` /
   `build_compiler_rt.sh` → `riscv64-unknown-aegir-musl`.
5. **libc++.** Promote `_LIBCPP_AEGIR` (today a build flag;
   `0002-aegir-path-grammar.patch` turns it on) to the `__aegir__` trigger, so the
   path grammar and the platform config follow the triple rather than a `-D`.
6. **The acceptance.** `scripts/targets.py`'s compile line `--target=…` →
   `riscv64-unknown-aegir-musl`, and verify on the guest.

## Constraints (AGENTS.md, enforced by the user)

- **Never edit vendored trees in place.** Patches under
  `third_party/patches/<project>/` are the source of truth; `apply_patches.py`
  and `scripts/check_pins.py` must pass.
- **No arbitrary or hardcoded limits**; capacity grows on demand.
- **No seL4 errors or warnings** in the log.
- **No flakes** — a broken run is a broken run; re-run to confirm.
- `make`/`make run` under `timeout`; afterwards
  `ps -eo args | grep '[q]emu-system'` must be empty.
- **STOP and ask for clarification**; never unilaterally change the plan.
- Run logs contain NUL bytes — use `grep -a`.

## Watch out for

- **LLVM rebuilds are expensive.** `scripts/build_llvm.sh` skips if
  `out/runtime/llvm-install` exists; `rm -rf out/runtime/llvm-install` to force
  one after an LLVM patch lands, or the run will use stale libraries (this bit
  the last session — `AEGIR-SVV` survived a reverted patch until the rebuild).
- The diagnostic baseline is **0 `AEGIR-SVV`, 0 trace prints**. Keep it.
- The user is comfortable carrying out-of-tree patches (LLVM is vendored, pinned
  at 20.1.8, and updates are rare) — so prefer the honest triple over an in-tree
  lie, and record each patch in `specs/third_party.md`'s upgrade list.

## Read first

- `specs/clang-on-aegir.md` — "The target triple" and "The hosted tier's input
  triple" (the decision, the measurement, the file list)
- `specs/development.md` — the `Sys:Development` tree; note `Include` is staged
  empty today (`run_target.py`'s `development_tree`), and the sysroot's headers
  are the piece *after* this one
- `specs/build.md` — the pinned ABI (`rv64imafdc_zicsr_zifencei` / `lp64d`)
- `specs/third_party.md` — the patch and upgrade rules
- `AGENTS.md`

## Related, not this task

`Sys:Development/Include` is created but empty — `scripts/sysroot-hello.c`
declares `write` instead of including `<unistd.h>` for exactly that reason. The
headers exist in the runtime (`out/runtime/musl-install/include`,
`out/runtime/cxx-install/include/c++/v1`); staging them and putting them on the
search path (`-isystem`, `--sysroot`, or the ToolChain default from step 2) is
the natural follow-on once the triple is real.
