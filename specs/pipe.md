# pipe: the PIPE: volume and the pipeline

Status: decided (2026-09). Aegir's byte pipe: a volume named `PIPE:`, like
`NIL:` a device rather than a disk (`specs/vfs.md`), and the shell's `|` that
connects one command's output to the next command's input. The two are one
arc because a `|` with no pipe to carry its bytes is a parser, and a pipe with
no `|` is a device nobody can reach.

The Amiga has no `|` operator: it has the `PIPE:` device, and a pipeline is
`Run >PIPE:name producer` then `Type PIPE:name`, two commands the user starts
by hand. Aegir keeps the device and adds the operator over it, the way the
Amiga's `>` became a shell redirection (`specs/shell.md`): the operator is
sugar, and what it stands on is a volume a session can also reach by name.

## The decisions

- **`PIPE:` is a volume, owned by a pipe service.** It registers itself with
  the VFS exactly as `NIL:` does (`specs/boot.md`, `specs/vfs.md`): it is
  handed no device, serves the volume protocol (`aegir/volume.h`) from its own
  memory, and is public, so a session and the commands it starts resolve
  `PIPE:name` through the namespace without a binding of their own. It is a
  device, not a filesystem: there are no directories, and a name is a pipe.
- **A pipe is named, created on first open, and dies when both ends close.**
  Opening a name for writing creates the pipe if it is not there; opening it
  for reading attaches to it (and creates it too, so a reader that starts
  first is not refused -- its reads wait). The pipe lives until every handle
  on it has closed. A name reused after that is a new pipe.
- **The pipe is a byte stream with an explicit offset, not a cursor.** Its
  reads are the volume protocol's (`kMethodReadHandle`), an offset and a
  count; its writes append. A read at an offset that reaches the current end
  answers one of two things: while the writer still holds its end, *no data
  yet* -- count zero and the end-of-file flag clear -- and the runtime waits
  and asks again; once the writer has closed its end, end-of-file -- count
  zero and the flag set. The distinction is the protocol's already: a regular
  file never answers count-zero-with-flag-clear, because a read past its end
  is its end.
- **The buffer grows on demand from the service's memory grant.** A write
  that the buffer cannot take answers a short count; the runtime waits and
  writes the rest. The grant is the pipe's ceiling, not a per-pipe constant:
  the service carves its buffer in chunks from a free list that spans the
  whole grant, so a pipe may be as large as the grant is and many pipes share
  it.
- **`|` connects two programs that run at once.** The shell parses a line
  into stages and hands them to the terminal, which holds the spawn authority
  (`specs/shell.md`'s Phase 6). The terminal spawns every stage in one command
  bracket, names a pipe between each pair, and gives stage *i*'s output to
  stage *i+1*'s input. Both run concurrently: the producer writes, the
  consumer reads, and the pipe buffers what separates their rates. A stage
  that names its own redirection keeps it at the pipeline's ends -- the first
  stage's input and the last stage's output -- and a middle redirection is
  refused, because the pipe is what connects it.
- **The pipeline's status is its last stage's.** Every stage reports its exit
  through the shell's console stream (`specs/shell.md`); the terminal counts
  the exits, and only when the last stage has reported does the shell's wait
  end, with the last status it saw. The last stage is the consumer, which
  cannot exit before its upstream closes its end, so the last exit is its.
- **A built-in cannot be a stage.** A built-in is the shell's own code, not a
  program the terminal can start (`specs/shell.md`); a pipeline whose stage
  is a built-in is refused. `echo` in a pipeline is the `Echo` *program* when
  one exists, not the built-in -- the shell's word, like any other, resolves
  to `C:` first only when it is not built in.

## The shape

### The PIPE: wire

The pipe service answers the volume protocol's methods, with the pipe's
semantics where a filesystem has none:

- `open` -- a path and mode flags. `read` (`kOpenRead`) attaches a read
  handle; `create` (a write open, `kOpenCreate`) creates the pipe and attaches
  a write handle, and truncates a name already there to nothing. The answer is
  the handle, zero the refusal.
- `read-handle` -- a handle, an offset, a count. The answer is read's:
  the bytes available at the offset, and the end-of-file flag. Past the
  current end with the writer open, count zero and flag clear; with the writer
  closed, count zero and flag set.
- `write` -- a handle, a count, the bytes. The bytes append to the pipe; the
  answer is the count taken, less than asked when the grant is full.
- `close` -- a handle. A write handle's close is the writer's end: every
  later read that reaches the end sees end-of-file. The pipe dies when the
  last handle closes.
- `stat` -- every name is a file that is there (the `NIL:` rule), so an open
  reaches it. `list` answers nothing; `mkdir`, `remove`, `rename`, `truncate`
  and `reap` are refused.

A handle is scoped to the caller's badge, as the volume protocol says; the
commands of one session share the terminal's badge, so a stage's writer and
its reader name the same pipe across that badge.

### The pipeline wire

The shell asks the launcher to start a pipeline with the launcher protocol's
pipeline method (`specs/launch.md`, `kMethodPipeline`): a stage count, each
stage's argv (the command words NUL-separated) and its own redirections, then
the context once -- the current directory, the environment and the path. The
launcher spawns the stages in one bracket and names the connecting pipes
itself -- `PIPE:p<serial>_<i>` -- so the shell never invents a pipe name and a
pipeline's bytes never depend on a name the user chose. The completion the
shell waits on is the existing `command status` call on the stream; the
launcher answers it only when every stage has reported.

### The service's memory

The pipe service is a freestanding service with a memory grant
(`memory_kib`), like the filesystems. It carves the grant into fixed-size
chunks and keeps a free list of them; a pipe's buffer is a chain of chunks,
and a write takes a chunk when the chain's tail fills. A read walks the chain
to its offset. Freeing a pipe returns its chunks to the list. The chunk size
is a service detail, not a protocol one; the grant is the ceiling, and the
list spans it, so the capacity grows with the grant rather than with a
constant.

## The phases

- **Phase 1 — the PIPE: volume.** The service, the manifest entry, the
  director's port rights, and the runtime's wait-and-retry so a reader on an
  empty pipe blocks rather than reads end-of-file. Provable without `|`: a
  command run with `>PIPE:x` and a later `Type`-style read of `PIPE:x` through
  the same session.
- **Phase 2 — the `|` operator.** The shell's parse of a line into stages,
  the pipeline method, the terminal's concurrent spawn and completion count,
  and the runner's `|` key. The acceptance runs a two-stage pipeline whose
  second stage is `aegir-read` (the fd-0 proof of `specs/shell.md`), cued by
  the second stage starting and exiting.
- **Phase 3 — a blocking read, and many pipelines.** The first pipe service
  is non-blocking: a reader waits by yielding to the writer, and the service
  serves one call at a time. A true block -- the reader's reply held until the
  writer moves (specs/signal.md) -- and a service that serves several pipes'
  ends at once (a thread, or deferred replies, `aegir::ipc::Owner`'s open
  question) are this phase. The protocol does not change.

## What this is not

- **A POSIX pipe.** No `|&`, no `2>`, no process substitution. The operator
  is the Amiga device with a connector, and the stages are programs.
- **A socket, a FIFO across sessions, or a named channel with a lifetime.**
  A pipe is a session's, by its name and the badge it is opened with; a
  `PIPE:` that outlives the commands that made it, or one two sessions share,
  is not this arc.
- **`Run` and `NewCLI`.** The process words (`specs/shell.md`) start a command
  in the background or a second console; a pipeline starts its stages, but
  neither word is here.

## Acceptance

Phase 1's: the runner types a line that writes to `PIPE:` and a later line
that reads it back, so the volume's create/append/close/read path is proven
without the operator.

Phase 2's: the runner types `type Sys:S/Shell-Startup | aegir-read`, a
two-stage pipeline whose producer is a `C:` program writing to its standard
output and whose consumer is `aegir-read` draining its standard input to the
console. `aegir-read` starting is the proof the terminal spawned both stages
and connected the pipe; its exit is the proof the consumer drained the
producer's output. `make check-script` grows no case -- the operator is the
shell's, and the parser is a line split -- and the pipe service's own read and
write are exercised by the same run.
