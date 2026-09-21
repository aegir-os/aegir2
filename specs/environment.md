# The process environment

Status: for review (2026-09). This is the spec the process-environment arc lands
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

- **The environment is per-process, and given at spawn.** Arguments, environment
  strings and the current directory are the process's own; nothing else sees
  them. The spawn `Request` grows them, and the bootstrap block names what the
  child was given, the way it names its ports.
- **Arguments are a list of strings, not a command line.** `main(int argc,
  char **argv)` is what a C++ program is written against, and the runtime
  already carries the C ABI; the Amiga's single command line is a parsing
  convention a program can build on top, not a thing the spawner should force.
- **The current directory is a VFS path, not a lock.** It is
  `Volume:component/path` — the VFS's own shape (`specs/vfs.md`) — held as a
  string. The VFS is stateless by path, so a lock would pin what the VFS does
  not hold; a string is what a process can carry, compare and hand on. A
  relative path resolves against it; an absolute path (`Volume:…`) never does.
- **A process with no current directory refuses a relative path.** The spawner
  sets it: a session's is `Home:`, a system service's is `Sys:`, and a process
  given none has none — a relative path is `-ENOENT`, an absolute path is always
  fine. The default is the spawner's, not the runtime's, so the policy lives
  where the process's identity does.
- **Environment variables are the process's own strings.** `getenv`/`setenv`
  read and write them; a child inherits the spawner's if the spawner passes them
  on. The Amiga's global, persistent `ENV:` volume and `GetVar`/`SetVar` are a
  different shape — a shared environment — and are a later arc if they are
  wanted at all.
- **The library is `aegir::environment`.** It parses the bootstrap block once
  and answers `argc`/`argv`, `getenv`/`setenv`, and
  `current_dir`/`set_current_dir`. A program includes it and never sees a slot
  or a syscall.
- **The C and C++ mapping is the filesystem arc's.** `getcwd`/`chdir` and
  `std::filesystem::current_path()` reach the same state; the runtime answers
  them there, not here.

## The shape

### What the spawner gives

`Request` grows three fields: the arguments (a list of strings), the environment
(a list of `NAME=VALUE` strings), and the current directory (a string, or empty
for none). The spawner copies the bytes into the child — a region it maps beside
the stack, or into a page of the image — and the bootstrap block carries an
entry naming the region and its length, so the child finds it the way it finds
its ports. The encoding is the spawner's and the library's to agree on; the
block says where, not what.

A process the director starts gets the manifest's arguments and a `Sys:`
current directory; a session gets auth's (a login has no arguments, and its
current directory is its `Home:`). Both are the spawner's choice, and the
process cannot tell which spawner it had.

### `aegir::environment`

One header, no seL4 in a caller's translation unit:

    namespace aegir::environment {
        int argc() noexcept;
        char const *const *argv() noexcept;
        char const *getenv(char const *name) noexcept;
        bool setenv(char const *name, char const *value) noexcept;
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

A global, shared environment (`ENV:`); a command-line parser (`ReadArgs`'s
shape); file descriptors or a process's open files; `$HOME` or any other named
variable the system sets for a program — the arc gives the mechanism, and a
program or a spawner decides the names.

## Acceptance

A spawned client (`apps/aegir-env-smoke`, the cxx-smoke's shape) reports what it
was given: `argc`/`argv` read back the spawner's arguments; `getenv` reads one
the spawner set and `setenv` one it sets itself; `current_dir()` is the
spawner's, `set_current_dir` changes it, and a relative path with no current
directory is refused. It prints `ENV_SMOKE_OK`. The director gives it arguments
and `Sys:` so the checks have something to read, and the flag-off build is
unchanged.
