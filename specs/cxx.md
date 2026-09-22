# The hosted C++ runtime

Status: decided (2026-09). This is the spec the runtime lands under.

Aegir's userland is C++, and it began as *freestanding* C++: no `operator new`,
no `<vector>`, no standard library (`specs/build.md`'s C++ bullet). This arc
gives the userland that asks for it a real one — the C++ standard library, a
freeing heap, and the glue between them — while the root task stays exactly as
it was.

## The decision

- **Director does not move.** The root task keeps `sel4runtime`, seL4's
  `musllibc` and `aegir-cxx-policy-freestanding` (`specs/director.md`). Nothing
  in this arc touches it. The same is true of the ordinary services and drivers,
  which are freestanding unless they opt in.
- **Hosted userspace is on by default, and gated.** `AEGIR_HOSTED_CXX` (default
  ON) builds the runtime — vendored full musl, libc++, `aegir-heap` — and the
  acceptance client that proves it (`apps/aegir-cxx-smoke`). `AEGIR_HOSTED_CXX=0`
  selects the lean freestanding build (director and the services).
- **The GUI toolkit is its own switch.** `AEGIR_TOOLKIT` (default ON, requires
  `AEGIR_HOSTED_CXX`) builds `aegir-trinket`, `aegir-bureau`, `aegir-datatypes`
  and `aegir-classes`, and the real greeter and bureau. While it is OFF those
  two programs build freestanding placeholders, because director refuses to boot
  when a manifest entry names a binary the initrd does not carry
  (`apps/aegir-director/src/services.cc`). The toolkit builds and the greeter is
  its first real client — it speaks the console protocol, loads an embedded
  Terminus font, and its form is drawn and read back by the acceptance script
  (`specs/trinket.md`). The window-manager and menu servers are still stubs; the
  switch is what keeps the runtime's own proof separate from that work.
- **The C library is the vendored upstream musl, not seL4's fork.** The seL4
  build's `projects/musllibc` is a musl 1.2.5 fork configured without locale,
  iconv or threads and with the `oldmalloc` backend. The hosted runtime needs
  the real library, so it vendors upstream musl 1.2.6 (`manifests/aegir.xml`),
  built per target by `scripts/build_musl.sh` with its default backend,
  **mallocng**, and with the syscall redirection that makes musl usable on seL4.
- **The standard library is libc++** (libc++abi and libunwind with it), from the
  vendored LLVM project, built per target by `scripts/build_libcxx.sh`. An
  earlier draft of this spec chose GCC's libstdc++ to match the compiler; that
  is overridden deliberately. libc++ is the more modern library, and it is the
  one whose ABI and headers this tree already carries.
- **Both libcs coexist, in different binaries.** No binary links two. Director
  and the freestanding services link seL4's `muslc`; hosted targets link the
  vendored full musl. A library never chooses a libc at all (below).

## The pieces

### `musl_full`: the C library

`scripts/build_musl.sh` builds `projects/musl` out of tree into
`out/<target>/musl-install` and `libs/aegir-musl/imported.cmake` exposes it as
the imported target `musl_full`. Two things about the build are load-bearing:

- **The syscall redirection is a tracked patch**
  (`third_party/patches/projects/musl/0001-riscv64-redirect-syscalls-to-sel4-vsyscall.patch`).
  Upstream musl's RISC-V syscalls are raw `ecall`s, which seL4 does not answer;
  the patch routes every one through the `__sysinfo` function pointer, which
  `aegir-heap` installs. The build refuses to run without it rather than produce
  a library that faults on its first syscall.
- **mallocng, not `oldmalloc`.** The seL4 fork chooses `oldmalloc` because its
  shim has a no-op `munmap` and no `mremap`. `aegir-heap` provides real
  `brk`/`mmap` over the process's own memory, so the default backend — the one
  that actually returns memory — is the right choice.

### `cxx`, `cxxabi`, `unwind`: the standard library

`scripts/build_libcxx.sh` builds the LLVM runtimes against the target's musl
headers and `libs/aegir-libcxx/imported.cmake` exposes them. The configuration
is **tier 1**, and each choice is a decision:

| option | value | why |
| --- | --- | --- |
| `LIBCXX_ENABLE_EXCEPTIONS` | OFF | tier 1; see "Deferred" |
| `LIBCXX_ENABLE_RTTI` | OFF | tier 1; see "Deferred" |
| `LIBCXX_ENABLE_THREADS` | ON | so `<thread>`/`<mutex>` compile and link; a thread does not start (below) |
| `LIBCXX_HAS_PTHREAD_API` | ON | the thread API is musl's |
| `LIBCXX_ENABLE_FILESYSTEM` | OFF | the VFS is a service, not stdio (`specs/vfs.md`) |
| `LIBCXX_ENABLE_LOCALIZATION` | OFF | see "Deferred" |
| `LIBCXX_HAS_MUSL_LIBC` | ON | the C library is musl |
| `LIBUNWIND_IS_BAREMETAL` | ON | there is no `dladdr` and no dynamic loader |

Exceptions and RTTI are off to match `aegir-cxx-policy-hosted`: libc++'s
`_LIBCPP_ODR_SIGNATURE` embeds the exceptions choice
(`libcxx/include/__config`), so a library built with them and user code
compiled without is an ODR mismatch, not merely a link question. `-D_GNU_SOURCE`
is what makes musl's headers declare the POSIX surface libc++ uses;
`_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS` works around GCC 14's unusable
`__remove_pointer` builtin.

`operator new`/`delete` are **libc++'s own** (weak definitions in
`libcxx/src/new.cpp` forwarding to `malloc`/`free`); the runtime does not define
them, because defining one form and not the others breaks libc++'s nothrow
consistency.

### `aegir-heap`: the freeing heap

`libs/aegir-heap` is where musl's memory syscalls meet Aegir's memory. Its
shape is the established one, taken from `libsel4muslcsys` and re-implemented
without the `libsel4utils` dependency:

- **musl's `libc` is never initialised** — `sel4runtime` runs constructors
  directly and never calls `__libc_start_main` — so a constructor seeds the two
  fields mallocng reads: `libc.page_size` (its alignment) and `libc.auxv` (the
  vector `get_random_secret` walks), from `sel4runtime_auxv()`. The same
  constructor points `__sysinfo` at the dispatcher, before any constructor can
  allocate.
- **`init()` claims the top of the process's address window** (the `Scratch`
  from `aegir-mem`) as the heap's budget: `brk` grows up from the bottom of the
  claim, `mmap` down from the top, and they meet in the middle. Pages are carved
  from the process's delegated untyped and mapped into its own VSpace
  (`Scratch::map_at`).
- **The dispatcher answers** `SYS_brk`, `SYS_mmap`, `SYS_munmap`, `SYS_mremap`,
  `SYS_madvise`, `SYS_write`, `SYS_writev` and `SYS_exit`/`exit_group`;
  everything else is `-ENOSYS`, which musl reads as errno. `SYS_writev` matters
  more than it looks: musl's `vfprintf` uses it, and without it libc++'s
  verbose-abort message is lost and an abort looks silent.
- **`munmap` is a no-op and `mremap` refuses.** The pages stay mapped — the
  address space is committed to the heap — and mallocng's groups reuse the small
  pieces. Returning frames to the kernel is a later refinement, not a
  correctness need.

### `aegir-c-headers`: a library does not choose a libc

A library needs C *headers* (`<stdint.h>`) to compile; it must not drag a C
*archive* onto the final link, because the tree has two libcs. So libraries link
`aegir-c-headers` (an interface target carrying only seL4 musl's include
directory) and the **final target** links exactly one libc — `muslc` or
`musl_full`. Before this, every library linked `muslc` publicly, and a hosted
target would have pulled seL4's archive in beside the full musl.

### The build policy that makes the frames small

`aegir-cxx-policy-hosted` compiles the hosted userland **`-O2`**, and that is not
optional. libc++'s containers are `always_inline`, and at `-O0` GCC gives every
inlined call its own stack slots and never reuses them: one `unordered_map`
insertion made a **33 KiB** stack frame and overflowed a service's stack. At
`-O2` the same function is a few hundred bytes. This is what Linux and BSD do —
their packages and their libc++ are built optimized — and it is why the problem
does not exist there.

GCC at `-O1` and above then trips a `-Wmaybe-uninitialized` **false positive** in
libc++'s `__hash_table`: `__chash` is always assigned, but GCC loses the
assignment across the `__rehash_unique` call. The tracked patch
(`third_party/patches/projects/llvm-project/0001-hash-table-initialize-chash.patch`)
initializes it, which is what upstream does. It is a code fix, not a warning
mask, so it does not run against the rule that warnings are fixed rather than
silenced.

The runtime pieces are built before configure by `scripts/run_target.py`'s
bootstrap when `AEGIR_HOSTED_CXX` is on, and both switches are always passed
explicitly so the CMake cache cannot keep a stale one.

## Boundary rules the arc established

- **A library does not choose a libc.** Headers travel with libraries; the
  archive is the final target's. (`aegir-c-headers`, above.)
- **seL4's C headers are not automatically usable from C++.** libsel4's RISC-V
  `syscalls.h` declares `strcpy` with C++ linkage
  (`kernel/libsel4/arch_include/riscv/sel4/arch/syscalls.h:837`), which clashes
  with musl's C declaration the moment libc++'s `<string>` pulls in `<cstring>`
  in the same translation unit. The fix is structural: a translation unit is
  either seL4-facing or libc++-facing, never both.
  `apps/aegir-cxx-smoke/src/checks.h` says why.
- **A service's stack is a spawn parameter.** `manifests/services.manifest`'s
  `stack_kib` becomes `Request::stack_pages` (default 8 KiB). The standard
  library's templates need more than the floor in some builds, and a stack is
  the spawner's to size, not a constant (`specs/userland.md`).
- **The allocator's untyped table holds live records only.** Every
  `alloc_object` leaves a record the kernel will never let be retyped again;
  `Allocator::release_entry` compacts a consumed record away (swap the last live
  entry in, shrink), so the fixed table bounds *fragmentation*, not objects ever
  allocated (project rule: capacity grows on demand).

## The completion program

Status: decided (2026-09). The runtime's remaining parts are one program,
worked in order, each its own arc with its own acceptance. Two framing
decisions run through it:

- **The standard library is completed for what Aegir is, and the calls are
  wrapped.** Filesystem access is a normal part of the standard library, on by
  default: `std::filesystem` works, and its paths are Aegir's (`Volume:…`,
  `specs/vfs.md`), so no program maps anything. Beside it, an Aegir library
  `aegir::filesystem` wraps the VFS's own calls — enumerating volumes, the
  things `std::filesystem` has no path for — so a program that wants Aegir's
  filesystem does not call seL4 or the namespace port by hand
  (`specs/userland.md`: the calls belong in a library, not a program).
- **POSIX is not the model, and it is not the mechanism.** `std::filesystem` is
  C++, not POSIX; it reaches the filesystem through the runtime. A POSIX
  emulation layer, ixemul's shape, is a separate, later thing for the *rest* of
  POSIX — `fork`, signals, pipes, a `/`-rooted mount table — that a
  Unix-targeted program links. It is not what gives a program `std::filesystem`.

The order:

1. **The process environment** (`specs/environment.md`). Arguments, environment
   and a current directory: the last is what `std::filesystem`'s relative paths
   and `current_path()` resolve against, and the library
   (`aegir::environment`) is the first working of the wrap-the-calls rule. Its
   own arc, first, because the filesystem arc depends on it.
2. **`__cxa_atexit` at process exit.** `sel4runtime` runs `__fini_array` and
   never calls libc's `exit`, so destructors registered through libc's
   `__cxa_atexit` do not run. A process that needs them at exit wants the
   `__funcs_on_exit()` bridge. Small, and it closes a correctness hole the
   other parts would otherwise each work around. **Landed**: the bridge is a
   constructor in `libs/aegir-runtime/src/debug.cc` (that translation unit,
   not one of its own, because a static archive only pulls an object the
   linker has a reason to), installing `__funcs_on_exit` and `__stdio_exit` as
   sel4runtime's pre-exit step and a clean halt as the exit itself; the
   cxx-smoke registers a handler and returns from `main`, and the acceptance
   checks `CXX_ATEXIT_OK` after `CXX_SMOKE_OK`.
3. **Exceptions and RTTI.** Tier 1 compiles without them. Turning them on means
   rebuilding libc++ with `LIBCXX_ENABLE_EXCEPTIONS`/`RTTI` on and proving
   unwinding in a *spawned* process — `.eh_frame` mapped and frame registration
   reached — before anything relies on it. The toolkit's widget classes are the
   first likely consumer.
4. **Threads.** `<thread>`/`<mutex>` compile and link against musl's pthread,
   but a thread does not start: `pthread_create` reaches `clone`, which the
   dispatcher refuses, and with exceptions off `std::thread`'s constructor would
   abort. `aegir-trinket`'s `WorkerPool` therefore does not spawn. A real
   thread is one seL4 TCB in the process's own address space, and
   `specs/userland.md`'s threading section already records the rules (tp, gp, a
   stack that does not overlap the TLS block).
5. **The filesystem.** `std::filesystem` in the runtime, and `aegir::filesystem`
   beside it. The runtime answers the file calls with Aegir-path semantics, and
   a tracked libc++ patch gives `path` the Aegir grammar, modelled on its
   Windows one — a root-name `Volume:`, always absolute, `/` the separator — so
   `is_absolute`/`root_name`/`absolute` are right. `aegir::filesystem` wraps the
   namespace (`volumes()` from count/describe, and resolve/list/read) for what
   `std::filesystem` has no path for; a union (`specs/namespace.md`) is served
   by the VFS, so the library is a thin wrapper and not a merge engine. Depends
   on 1 for the current directory.
6. **Locale, iconv and BiDi/RTL.** The toolkit keeps `locale.cc` in its build —
   its C dependencies are musl's — while `translation.cc` and `bidi.cc` are
   gated out because they are stubs, not because they cannot compile. The
   locale arc turns musl's locale on and implements UAX #9 for real.
7. **The compiler choice.** GCC builds everything today. Clang 22
   cross-compiles the hosted code cleanly and compactly (224 bytes at `-O0`,
   112 at `-O2`) and would not need the `__chash` patch at all, because libc++
   is Clang's library. Switching is `specs/build.md`'s deferred decision and
   its own arc; it needs `lld` and a build-system change (or the hybrid build
   that document already describes).

## Acceptance

`apps/aegir-cxx-smoke` is the acceptance client. It is a spawned boot service
that adopts its untyped and window, stands the heap up, and reports each check —
`malloc`, a page written and read back, `std::string` growing, `std::vector`
growing, `std::unordered_map` allocating and looking up, and a freed chunk
allocated again — then prints `CXX_SMOKE_OK`. It is deliberately independent of
the toolkit, so a toolkit still being repaired cannot make the runtime look
broken. It runs on the 8 KiB stack floor.

The flag-off build and boot (`AEGIR_HOSTED_CXX=0 make build && make run`,
`AEGIR_BOOT_OK`) are unchanged by any of this and stay the checkpoint.
