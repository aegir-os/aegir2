# The pager: demand-grown stacks, and the seed of paging

Status: decided (design), for implementation. Extends `specs/memory.md` and
`specs/launch.md`; supersedes nothing.

Aegir has no paging. `aegir-spawn` maps a child's whole image and a fixed,
eagerly-mapped stack up front, and nothing maps a page a running process did not
already have. This is the first case that needs one, and the place to build it is
a **pager** — a task that serves a thread's fault endpoint and maps the page it
faulted on. There is no kernel paging in seL4, so "paging" is a user task; the
pager is that task.

## Why, measured

The on-device compiler overflowed its stack while parsing C++ (a libc++-including
`hello.cc`). `aegir-spawn` lays a child out as

```
[image][bootstrap block page][IPC buffer page][stack, growing down]
```

(`libs/freestanding/aegir-spawn/src/process.cc`: `block_at = align_up(load_end)`,
`ipc_at = block_at + kPage`, `stack_lo = ipc_at + kPage`), so the stack's growth
region is *below* it, over the two pages the process needs most. What seL4
reported:

```
launcher: command faulted, fault 5 VMFault ip=0x38db66e addr=0x480dee8 fsr=0x7
```

`fault 5` is `seL4_Fault_VMFault` (`sel4/sel4_arch/.../types_gen.h`), the
instruction is `clang::Parser::TryAnnotateTypeOrScopeToken`'s prologue store
`sd ra, 0x1d8(sp)` with `sp = 0x480dd10`, and the address is `_end + 0xee8` — the
first page *past the image*. The stack order was:

```
image _end 0x480c818   block 0x480d000   IPC 0x480e000   stack [0x480f000,0x481f000)
```

so the parser's frame walked **through the IPC buffer** (clobbering this
process's own IPC buffer — which is what every `no-cap` namespace resolve before
the fault was: a symptom, not a capability bug) and **into the read-only block
page** (the VMFault). The default is `kDefaultStackPages = 16` (64 KiB)
(`aegir-spawn/src/process.cc`).

Two faults of shape, one root cause: **an eager, small stack whose growth region
is precious memory, with no guard.**

## Decisions

### 1. The layout and the guard

Put the **bootstrap block and IPC buffer above the stack**, and keep an
**unmapped guard page below** the stack's mapped bottom:

```
[image][guard][stack, growing down ...][block][IPC]
```

The child already learns `block_at`/`ipc_at` from the start frame
(`build_start_frame(..., block_at, ipc_at)`) and the IPC address from its TCB
configure, so this is arithmetic in one place. The guard is what makes an
overflow a fault *at the stack's own boundary* instead of a corruption that
surfaces somewhere else. This is right whatever else is decided, and lands first.

### 2. The stack grows on demand, bounded

Reserve a stack **range**, map only the initial pages, and grow down on a fault
in the reserved-but-unmapped part: allocate a frame, map it, reply to the fault
endpoint to resume. Past the bound — the reserved base, or the declared max — it
is an overflow, not a growth.

The numbers are FreeBSD's, because FreeBSD is the clang-on-a-real-OS control
(`sys/amd64/include/vmparam.h`, `vm_map.c`'s `vm_map_growstack`):

| | value | meaning |
|---|---|---|
| initial mapped | 16 pages (64 KiB) | the existing floor, `kDefaultStackPages` |
| max | 8 MiB | `DFLSSIZ`, the default limit |
| hard ceiling | 512 MiB | `MAXSSIZ`, an operator may raise the default to it |
| growth step | 128 KiB | `SGROWSIZ` |
| guard | 1 page | `security.bsd.stack_guard_page` |

### 3. The pager is the spawner, and owns no memory

A child's fault endpoint is set by its spawner, so **the pager is the spawner**:
the director for boot services, the launcher for commands. It retains the child's
**VSpace root** — which it had at spawn and today gives away (`give_vspace`) —
and that is its *only* new authority: a mapping authority, not a memory one.

Frames come from **`memory`** (`specs/memory.md`) through a `mem.main` copy
**minted with the child's own id**, exactly as a grant is. So the stack's frames
are the child's, and one `release(child_badge)` reaps them with everything else
the child held — the pager invents no ledger and no reaping.

The child's own heap mapping is unchanged: it is in-process growth through the
window, not fault-driven. The pager may absorb it later, or not.

### 4. A stack overflow is a named, deliberate end

- fault in the reserved-but-unmapped stack range → grow;
- fault in the guard, at/below the reserved base, or past the max → **stack
  overflow**: report it by name with the numbers (`pc`, `sp`, `base`, `max`) and
  terminate through the supervisor's existing path (end the stream, `release`,
  the registry records the departure);
- any other fault → unchanged.

The overflow carries a **distinct end status** (a reason beside `kFaultStatus`),
so `wait`/the registry can say "terminated: stack overflow" and not just "fault".
Diagnostics are part of the fix (`AGENTS.md`: a refusal names what ran out, with
the numbers); "fault 5" is exactly what sent this arc's investigation down the
wrong path once.

### 5. The declaration: `PT_GNU_STACK.p_memsz`

ELF has exactly one stack-size field, and Aegir's binaries already carry it:
**`PT_GNU_STACK`**, whose `p_memsz` is the stack size GNU ld's `-z stack-size=`
writes. The prebuilt host `ld.lld` supports it, and `clang -Wl,-z,stack-size=<n>`
passes it through:

```
aegir-clang (today):  GNU_STACK ... MemSiz 0x000000 RW
-Wl,-z,stack-size=0x800000:  GNU_STACK ... MemSiz 0x800000 RW
```

So a binary declares its **max** with a standard link flag — no custom note, no
object library, no LLVM patch. `Aegir's` CMake passes it (one default, per-target
overridable); the loader reads `PT_GNU_STACK.p_memsz`; and a binary linked
outside CMake can pass the same flag. `p_filesz` is 0 on the host lld and is not
used. A custom ELF note is only needed if a binary ever wants to tune the
*initial/grow* per-binary, which nothing does today.

`PT_GNU_STACK.p_memsz` is the *process's* stack, not a thread's. musl reads it
for `__default_stacksize` (`projects/musl/src/env/__init_tls.c`), so the link
flag made every `std::thread` ask for the whole declared stack: a boot
service's single-level CSpace cannot hold an 8 MiB thread stack's 2048 frame
caps (`slots 4084 of 4084`), and the cxx smoke's `std::thread` died of `EAGAIN`.
Aegir's thread stacks are not the process's stack, so a tracked musl patch
(`third_party/patches/projects/musl/0004-PT_GNU_STACK-is-the-process-stack-not-a-thread-default.patch`)
leaves `__default_stacksize` at `DEFAULT_STACK_SIZE`; the declaration keeps its
one meaning.

Resolution order:

```
explicit launch request (`Stack`-style, the user's override)
    > boot manifest (`stack_kib`, boot services)
    > PT_GNU_STACK.p_memsz          <- the flag brands any host-linked binary
    > compiled-in default (16 pages initial, 8 MiB max, 128 KiB grow)
```

An absent declaration is a legal answer, not a failure: a binary with no note and
no flag gets the default, or the launcher's override.

## The page source is the one variable

The pager's one real interface is `populate(vspace, addr) -> source`:

- **anonymous** (the stack now, and `mmap`'s anonymous pages later): a frame from
  `memory`;
- **file-backed** (the ELF segments later — the win of not mapping an 85 MB image
  to touch 5 MB of it): the page read from the image/file through the VFS.

Build it as that interface rather than a stack branch, so the file-backed case is
a second arm, not a rewrite. The root task (director) is the top of the
recursion — no supervisor, so it is fully mapped by construction — and the boot
before the pager's sources are up (`memory`, the VFS) is fully mapped by
dependency order. "Everything pages" means everything after that, by default.

## The plan (each its own commit, green each time)

1. **Layout + guard.** Landed. Block/IPC above the stack; an unmapped guard page
   below. The overflow is one clean, named fault.
2. **`PT_GNU_STACK` read + the link flag.** Landed, with a tracked musl patch:
   musl reads `p_memsz` for `__default_stacksize`, so raising it there made every
   `std::thread` ask for the process's whole stack. The flag is now the pager's
   declaration only.
3. **The pager.** Landed, both halves. The stateless growth walk
   (`aegir-spawn`'s `page_stack`, `PagerSource`) is shared; the launcher's
   `ServiceKit` grows a command from a child-badged `mem.main` chunk retyped into
   a per-command L2 CNode run; director's supervisor grows a boot service from a
   child-badged `mem.main` chunk retyped into a **reserved pool** of its own root
   slots (`Allocator::reserve_slot_run`, half of what is free at supervisor
   start) — its own allocator, so the boot thread and the pager never race. A
   service is watched only once `mem.main` has been started, so the memory
   service and everything before it are not paged. The overflow is reported by
   name and end status, and the acceptance exercises it: `overflow` is a command
   linked with `-z stack-size=0x20000`, and the run checks the named report and
   `return code 129`.
4. **File-backed paging** (the ELF segments), later, through the same interface.

**Not done: the root's two-level CSpace.** Director is the one CSpace whose
`guard`/`radix` convention differs (the kernel's initial CNode, guard 48 /
radix 16), and the pager does not need it: the reserved pool gives the pager its
own slot range without re-rooting. The re-root stays on the table for
uniformity, with a measured cost — director holds 22368 caps, so `l2` would have
to be >= 15, making per-service CNodes ~512 KB each.

## Constraints

- **The pager's `alloc` is synchronous in the fault path.** Safe (memory does not
  depend on the pager), but it must be written down — the boot already deadlocked
  once on "a granter must not wait behind what it is granting"
  (`specs/memory.md`).
- **Retaining the VSpace root is a stated change** to `give_vspace`; one cap per
  live child, and the same change real paging needs.
- **The root task is not paged.** No supervisor above it; its image is placed by
  the ELF loader. Its own address space is what it maps.
- **Every seL4 line is a hard failure** (`AGENTS.md`). The launcher's fault line
  now names the fault's kind and, for a VMFault, its IP/address/FSR — keep it.
- **No arbitrary ceilings.** The max is declared (manifest/flag) or the FreeBSD
  default; nothing hardcodes a small stack.

## Open, for review

- **Per-spawn policy.** Landed as default-on: the launcher watches every
  command; director watches a service once `mem.main` has been started. There is
  no `full` request yet — the pre-source boot is simply not watched.
- **The end-status word** for a stack overflow: landed as `kStackOverflowStatus`
  (129) beside `kFaultStatus`, reported with `pc`/`sp`/`base`/`max`.
- **Whether the child's heap routes through the pager** or stays in-process:
  unchanged — it stays in-process, through the window.
