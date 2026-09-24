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
| `LIBCXX_ENABLE_EXCEPTIONS` | ON | exceptions arc (`specs/cxx.md`'s step 3) |
| `LIBCXX_ENABLE_RTTI` | ON | exceptions arc (`specs/cxx.md`'s step 3) |
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
3. **Exceptions and RTTI.** Tier 1 compiled without them; turning them on meant
   rebuilding libc++ with `LIBCXX_ENABLE_EXCEPTIONS`/`RTTI` on and proving
   unwinding in a *spawned* process. **Landed**: the cxx-smoke throws through a
   live frame object and the frame's destructor runs, and a `dynamic_cast`
   resolves, before it prints `CXX_SMOKE_OK`. Four things fell out of it:
   - **The policy split.** A library can no longer *export* `-fno-exceptions`:
     the negative flag lands after a hosted target's `-fexceptions` with no
     later positive flag to undo it, and the same link order that would fix
     that also orders libc++'s includes *after* musl's. So `aegir-cxx-policy`
     (what libraries link) is C++17 + warnings only; a library with sources
     sets the lean flags **privately** on itself; a freestanding target links
     `aegir-cxx-policy-freestanding`; a hosted one links
     `aegir-cxx-policy-hosted` first. The split is now structural:
     `libs/{freestanding,hosted}` and `apps/{freestanding,hosted}` each set
     their policy at directory scope, so a target's policy is where it lives
     rather than a per-target flag, and `aegir-cxx-policy-freestanding` is
     gone.
   - **libunwind is not linked.** The riscv64 bare-metal toolchain ships no
     `libgcc_eh`, but its `libgcc.a` carries `_Unwind_*` and the frames are
     registered by the `crtbegin.o` the link already has; libunwind's baremetal
     build wants linker-provided `__eh_frame_*` symbols, so libgcc's unwinder
     is the one used and libunwind is never reached.
   - **`aegir-cxxabi-shim`.** `__cxa_call_terminate` lives in GCC's libsupc++
     (libstdc++), which this toolchain omits and libc++abi does not define, so
     libc++'s `<string>` left a hosted link undefined; the shim is its ABI body.
   - **`theme.cc`.** Enabling RTTI exposed that `Theme`'s out-of-line virtuals
     were declared and never defined, leaving the class without a key function
     and its typeinfo undefined the moment anything needed it. The base hooks
     now have their no-op default bodies.
4. **Threads.** `<thread>`/`<mutex>` compile and link against musl's pthread,
   but a thread did not start, for two reasons -- both now addressed. First,
   hosted processes never
   ran musl's `__libc_start_main` -- sel4runtime calls `main` directly -- so
   `libc.can_do_threads` and `libc.tls_*` were never set and `pthread_create`
   refused with `ENOSYS` before it reached `clone`
   (`projects/musl/src/thread/pthread_create.c:249`). Second, `__clone` is
   hand-written assembly that issues a raw `SYS_clone` `ecall`, which the
   syscall patch does not cover (`arch/riscv64/syscall_arch.h` redirects only
   `__syscall0..6`), so it would not reach the dispatcher even then.
   `aegir-trinket`'s `WorkerPool` therefore did not spawn. A real thread is
   one seL4 TCB in the process's own address space, and
   `specs/userland.md:85` records the rules (tp, gp, a stack that does not
   overlap the TLS block). The order:

   1. **A thread-creation primitive** -- `libs/freestanding/aegir-thread`
      (landed): retype a `seL4_TCB` from the process's own untyped, write its
      user context with the three rules, name its IPC buffer in the TCB
      configuration (`seL4_TCB_Configure`, the same call the spawner uses for a
      process's boot thread -- libsel4 reaches the buffer through the TLS
      variable `__sel4_ipc_buffer`), and start it. The freestanding smoke is a
      check in `aegir-test`: a second thread runs, reaches a global and the
      console through its own TLS, and signals the starter. The architecture's
      half (the page object's name, the global pointer) is one header,
      `aegir/thread/arch.h`.
   2. **A thread in a hosted process** -- `aegir-cxx-smoke` (landed): the same
      primitive on the runtime this process actually uses. A second seL4 TCB
      with its own stack, TLS block and IPC buffer shares the address space,
      and the worker runs musl's `malloc` (whose syscalls reach the dispatcher
      from the new thread too) and reaches the console.
   3. **musl's TLS at hosted startup** (landed): `aegir-heap`'s `init` calls
      musl's `__init_tls` with the auxv flattened the way `__init_libc`
      flattens it, so `libc.tls_*` and `can_do_threads` are set and the
      process's thread pointer is musl's. libsel4's `__sel4_ipc_buffer` is
      carried across into the new TLS. This is what lets `pthread_create` past
      its first refusal. It runs from `init`, not a constructor, because the
      main thread's TLS does not fit musl's builtin static TLS and
      `__init_tls` allocates it through our own mmap.
   4. **The clone handler** -- `aegir-heap` (landed). `__clone` is raw assembly,
      so the musl patch (`third_party/patches/projects/musl/0002`) turns it into
      a tail call to `__aegir_clone`, which the heap provides. There is no
      second return: musl has already made the child's stack and its `tp` (its
      pthread struct), and the handler starts a real seL4 TCB there with the
      thread primitive. The function and its argument have nowhere in the
      Linux ABI to ride, and putting them on a stack the thread has not been
      given is a prologue away from being clobbered, so they are left in the
      unused part of the child's own IPC page and the trampoline is pointed at
      them. That frame also carries the child's TCB. Building the thread and
      starting it are two steps (`prepare`/`resume` in `aegir-thread`) for
      exactly this hand-off.
   5. **Ending a thread** (landed). musl's `__pthread_exit` finishes by calling
      `SYS_exit` in a loop. The dispatcher must end *this* thread, not the
      process, and it must do what the kernel's `CLONE_CHILD_CLEARTID` would:
      clear the address musl passed clone (the thread-list lock). The thread's
      own TCB, and that address, are recorded in its TLS by the trampoline --
      thread-local, because the process's boot thread has neither and its
      `SYS_exit` is the process's. The clear matters because Aegir has no
      futex: musl's `__wait` and the joiner's `__tl_sync` spin on that word, so
      clearing it is the wake. A thread that exits suspends its TCB; a
      killed or exited thread is not rebuilt, and its caps are not reclaimed.
   6. **`std::thread`** (landed). It falls out of the pieces above: the
      cxx-smoke starts a thread that joins and takes a `std::mutex` while it
      runs ("std::thread runs, joins, and takes a std::mutex") -- the mutex a
      proof of musl's lock over the thread's own TLS, the same class of check
      as the unwind.
5. **The filesystem.** `std::filesystem` in the runtime, and `aegir::filesystem`
   beside it. The runtime answers the file calls with Aegir-path semantics, and
   a tracked libc++ patch gives `path` the Aegir grammar, modelled on its
   Windows one — a root-name `Volume:`, always absolute, `/` the separator — so
   `is_absolute`/`root_name`/`absolute` are right. Depends on 1 for the current
   directory. The calls split by **linkability**, because the same VFS calls
   serve a freestanding service and the no-exception runtime dispatcher as well
   as a hosted program, and only the last wants exceptions:

   1. **The transport** — `libs/freestanding/aegir-vfs-client` (landed):
      `aegir::vfs`, the namespace's resolve/count/describe and a volume's
      read/list/stat/open/write/close/mkdir/remove over the two protocol
      headers, every call a value or a refusal. Freestanding so a service that
      holds `vfs.namespace` links it without the exception personality, and the
      runtime's dispatcher stands on the same calls.
   2. **The wrapper** — `libs/hosted/aegir-filesystem` (landed): 
      `aegir::filesystem`, hosted, with `std::filesystem`'s error model — a
      throwing overload and an `error_code` one — over the transport. Its
      first call is `volumes()`, the enumeration `std::filesystem` has no
      path for (capability-free, answered on the namespace port). The
      path operations that need a resolved volume capability wait for
      `std::filesystem` to want them, where the capability's slot and its
      reuse are decided.
   3. **The runtime's file calls** — `libs/hosted/aegir-heap/src/files.cc`
      (landed). musl's filesystem functions and `std::filesystem` issue
      Linux syscalls; `heap.cc`'s dispatcher answers them from the
      process's own memory, resolving Aegir paths through `aegir::vfs`
      instead of asking the kernel. A per-process fd table (grows on
      demand) holds each open file's volume capability, its
      volume-relative path, a write handle when it is open for writing, and
      its read cursor; a directory is the same minus the handle, with a
      listing cursor. `openat`, `close`, `read`, `write`, `lseek`,
      `newfstatat`/`fstat` (musl's kstat path, not statx), `getdents64`,
      `mkdirat`, `unlinkat`, `chdir`, `getcwd` and `fcntl` are the calls
      answered; each is a value or a negative errno. Transient resolves
      reuse one capability slot (the allocator never takes a used slot
      back, so the layer keeps its own free list), and the current
      directory is one buffer the runtime owns, so `chdir`/`getcwd`,
      `std::filesystem::current_path()` and `aegir::environment` read the
      same string. libc++ is built with `LIBCXX_ENABLE_FILESYSTEM=ON`; a
      tracked musl patch (`0003`) lets `getcwd` accept a `Volume:` root,
      which musl's own `/`-only check would otherwise refuse. This is what
      turns on `std::filesystem`.
   4. **The libc++ `path` patch** —
      `third_party/patches/projects/llvm-project/0002` (landed) gives `path`
      the Aegir grammar, modelled on libc++'s Windows one and turned on by
      `_LIBCPP_AEGIR` (defined by the libc++ build and by
      `aegir-cxx-policy-hosted`, because `path`'s predicates are inline in the
      header and must match the library's): a root name is a volume
      (`Name:`), `is_absolute()` is `has_root_name()` — "always absolute" —
      and `/` stays the separator. `root_name`, `relative_path` and
      `absolute` are right, and a relative path joins the current directory
      as `Volume:component` rather than `Volume:/component`.

   The acceptance is `apps/hosted/aegir-fs-smoke`, spawned with `needs =
   vfs.namespace`: it reads through both halves -- counting and describing
   volumes, reading and listing the initrd, listing the AEGIR FAT volume, and
   writing, reading back and removing a file on the scratch FAT volume, each
   of the read/list/stat calls proved with `aegir::vfs` -- and then runs
   `std::filesystem` over the same filesystem: `status`/`file_size`,
   `is_directory`, a `directory_iterator`, `create_directory` + `remove`,
   `current_path()`, and the grammar (`is_absolute`/`root_name`/`absolute`).
   It prints `FS_SMOKE_OK`.
   A finding from it, fixed in the same arc: a second client on a FAT volume
   used to corrupt the first, because every block-device client mapped one
   shared DMA window. Each client now reads through a window of its own
   (`aegir/block.h`; the partition manager carves one per filesystem).
6. **Locale, iconv and BiDi/RTL** (`specs/locale.md`). The toolkit keeps
   `locale.cc` in its build — its C dependencies are musl's — while
   `translation.cc` and `bidi.cc` are gated out because they are stubs, not
   because they cannot compile. musl's own locale and iconv are already in
   `musl_full`; the arc implements UAX #9 for real first, then turns libc++
   localization on (`LIBCXX_ENABLE_LOCALIZATION`), then gives the toolkit's
   `Locale` CLDR data and gettext a parser.
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
