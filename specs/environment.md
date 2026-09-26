# The process environment

Status: decided (2026-09). This is the spec the process-environment arc lands
under — the arc `std::filesystem` and `aegir::filesystem` wait on
(`specs/cxx.md`'s completion program).

A spawned process today is given a binary, a stack, ports, memory and a
bootstrap block, and nothing that says what it was asked to do or where it
stands. It has no arguments, no environment and no current directory. This arc
gives it those, because a program that takes arguments is the ordinary case, and
because `std::filesystem`'s relative paths and `current_path()` need a current
directory to resolve against. The whole of it is wrapped in a library, so a
program never reads the bootstrap block or touches seL4 to find out what it was
given (`specs/userland.md`: the calls belong in a library, not a program).

## The decisions

- **The environment is per-process and inherited.** A spawned process gets the
  spawner's arguments, variables and current directory, which the spawner may
  add to or override; nothing else sees them. The spawn `Request` carries them,
  and the bootstrap block names what the child was given, the way it names its
  ports.
- **Arguments are a list of strings, not a command line.** `main(int argc,
  char **argv)` is what a C++ program is written against, and the runtime
  already carries the C ABI; the Amiga's single command line is a parsing
  convention a program can build on top, not a thing the spawner should force.
- **The current directory is a VFS path, not a lock.** It is
  `Volume:component/path` — the VFS's own shape (`specs/vfs.md`) — held as a
  string. The VFS is stateless by path, so a lock would pin what the VFS does
  not hold; a string is what a process can carry, compare and inherit. A
  relative path resolves against it; an absolute path (`Volume:…`) never does.
  It is inherited like the variables, and the spawner sets it where the
  process's identity calls for it: a session's is `Home:`, a system service's
  `Sys:`.
- **A process with no current directory refuses a relative path.** A process
  given none has none — a relative path is `-ENOENT`, an absolute path is always
  fine. The default is the spawner's, not the runtime's, so the policy lives
  where the process's identity does.
- **The persistent environment is layered, as a union.** `ENV:` is a
  `specs/namespace.md` union — a name read as one directory — whose members are
  `Sys:Prefs/Env-Archive` (the base) and `Home:Prefs/Env-Archive` (first, so the
  user's overrides and appends), the user's the create target for an ordinary
  process and the system's for a system-authority one. A listing of `ENV:`
  through `aegir::filesystem` or `std::filesystem` returns the merged set; a
  write lands in the create target. A startup reads the merged view into the
  first processes' environments; the DOS toolset — `SetVar`/`GetVar`, once the
  shell and a `CON:` handler exist — is where the writes happen. This arc
  defines the per-process mechanism and the binding; the startup and the tools
  are later arcs. **Landed** (specs/shell.md's Phase 5): the in-memory half —
  `Set`/`Get` over `aegir::environment`, and `environ()` so a command inherits
  the shell's environment — and the persistent half. `auth` makes the home's
  `Prefs/Env-Archive` (owned by the user, so a write lands there) and binds
  `ENV:` for the session and terminal badges: the user's archive first (the
  create target), the system's base appended. The same home pass makes the
  user's `S:` script directory (`specs/auth.md`, `specs/shell.md`). The system archive ships in the
  image (`Sys:Prefs/Env-Archive`, `exitcode` 11). The shell reads the merged
  view once at startup (`load_environment`) into `aegir::environment`, and
  `Set` writes the variable to `ENV:<name>`, which lands in the create target.
  The startup and the tools are landed; a re-login that re-reads a changed
  archive is the shell's to prove when re-login exists.
- **The library is `aegir::environment`.** It parses the bootstrap block once
  and answers `argc`/`argv`, `getenv`/`setenv`, and
  `current_dir`/`set_current_dir`. A program includes it and never sees a slot
  or a syscall.
- **The C and C++ mapping is the filesystem arc's.** `getcwd`/`chdir` and
  `std::filesystem::current_path()` reach the same state; the runtime answers
  them there, not here. Landed with `specs/cxx.md` step 5: the runtime's file
  layer (`aegir-heap/src/files.cc`) owns the one buffer, and
  `aegir::environment` is its C++ face over a plain-C bridge.

## The shape

### What the spawner gives

`Request` grows three fields: the arguments (a list of strings), the environment
(a list of `NAME=VALUE` strings, normally the spawner's own), and the current
directory (a string, normally the spawner's own, or empty for none). The
arguments and the environment ride in the **startup frame** — the `argc`/`argv`/
`envp` the C ABI already carries, which the runtime reads and the library then
wraps — and only the current directory is a bootstrap block entry (`CurrentDir`),
because it is the one piece the frame has no place for.

Inheritance is the default, so a spawner that adds nothing passes its own
environment through: a shell starts a program, and the program sees the shell's
variables and current directory. A spawner that wants otherwise says so — the
director gives a boot service the manifest's arguments and a `Sys:` current
directory (its default), and auth gives a session a `Home:` current directory.
The process cannot tell which spawner it had.

### `aegir::environment`

One header, no seL4 in a caller's translation unit:

    namespace aegir::environment {
        int argc() noexcept;
        char const *const *argv() noexcept;
        char const *getenv(char const *name) noexcept;
        bool setenv(char const *name, char const *value) noexcept;
        // The whole environment as NAME=VALUE strings, NUL-terminated, this
        // process's own settings shadowing what it inherited: what a spawner
        // passes on.
        char const *const *environ() noexcept;
        // The current directory, or an empty string when the process has none.
        std::string_view current_dir() noexcept;
        bool set_current_dir(std::string_view path) noexcept;
    }

The strings live in the region the spawner mapped; the library parses it once,
on first use, and points into it. `setenv` and `set_current_dir` keep their
state in the library (the process's own memory), not in the mapped region, which
is read-only after the spawn.

### The current directory and the VFS

The current directory is not resolved until a path is: `aegir::filesystem`'s
relative operations join it to the argument (`specs/vfs.md`'s component path)
and hand the volume and the rest to the namespace. A process that changes its
current directory changes only its own string; nothing in the VFS is told, and
nothing in the VFS can change it.

## What this is not

The shell and the DOS toolset that manipulate the persistent environment
(`SetVar`/`GetVar`, and the `CON:` handler they need); a startup sequence that
reads `Env-Archive` into the first processes; file descriptors or a process's
open files; `$HOME` or any other named variable the system sets for a program
— the arc gives the mechanism, and a program or a spawner decides the names.
The command-line parser (`ReadArgs`'s shape) is `specs/dos.md`'s `aegir::args`,
which sits on this arc's `argc`/`argv`: this file says what a process was
given, that one says how a command reads it.

## Acceptance

A spawned client (`apps/aegir-env-smoke`, the cxx-smoke's shape) reports what it
was given: `argc`/`argv` read back the spawner's arguments; `getenv` reads one
the spawner set and `setenv` one it sets itself; `current_dir()` is the
spawner's, `set_current_dir` changes it, and a relative path with no current
directory is refused. It prints `ENV_SMOKE_OK`. The director gives it arguments
and `Sys:` so the checks have something to read, and the flag-off build is
unchanged.
