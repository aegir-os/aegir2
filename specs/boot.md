# boot: Startup-Sequence, and the boot session

Status: the boot session's success path is implemented (2026-09). This spec is
the boot arc's — the system's startup command file, the session that runs it,
and (in the next slice) the read-only view a failed boot leaves and the rescue
shell behind a boot flag. `specs/shell.md` is the command line that runs it;
this file is the boot's own shape.

An Amiga boots by running a command file: the first file on the boot disk,
`S:startup-sequence`, is handed to the Shell, which runs it. Aegir does the
same, with the multi-user split that a login forces: the system's sequence is
system authority and runs once, and a user's shell startup is the user's own
(`specs/shell.md`'s Shell-Startup).

## The decisions

- **Startup-Sequence is system-only.** `Sys:S/Startup-Sequence` is on the
  system volume, and editing it needs elevation. It is the `/etc/rc` role, not
  a user's profile: no `Home:S` copy shadows it, and the user's `S:` is a
  separate name (`specs/shell.md`).
- **It runs once, at boot, before the greeter.** `auth` spawns the boot
  session after it has read the user database and before `start_greeter`, so
  the sequence's effects are the system's before anyone logs in — which is what
  lets it affect the greeter.
- **The boot session is a terminal and a shell, as a session is.** It is the
  same `aegir-terminal` a login runs, spawned from the same kit, on a system
  badge (`system.boot`, `specs/services.md`), with `Sys:` as its current
  directory. Its shell is started with `Sys:S/Startup-Sequence` as its command
  file and runs it; a shell started with no file is interactive and runs
  Shell-Startup instead (`specs/shell.md`). Its memory is auth's own
  delegation, not the session pool, because it is not reclaimed: a failed boot
  leaves it standing (below).
- **The shell signals when the script is done, and auth waits.** The boot
  terminal is granted a notification (`boot.doorbell`) it passes to its shell;
  the shell signals it when the command file's frame empties or `EndCLI` takes
  it. `auth` waits on it and only then starts the greeter. A script that never
  ends would hold the boot, which is the same wait a service's ready is.
- **The quiet default is `EndCLI >NIL:`.** `NIL:` is a volume
  (`specs/vfs.md`), so the sequence's close is silent with no special case, and
  the boot window never appears unless the script writes.
- **The boot session's window is hidden until it has output.** The console
  shows a window only on its first damage (`specs/console.md`), and the boot
  terminal does not show its window at all: a quiet sequence leaves no window,
  and `>NIL:` keeps it that way. The visible failure view — the one that comes
  up when the sequence fails — is the next slice's.
- **`C:` is bound for the boot badge; `Sys:` is waited for.** A command in the
  sequence resolves through `C:` (`Sys:C`, `specs/dos.md`), which auth binds
  for the boot badge. `Sys:` is a filesystem's to register and comes up after
  auth, so the bind is retried until the boot volume is there — the same wait
  the user database's resolve does.

## The boot session

    auth:
        read the database
        wait for Sys:C, bind C: for the boot badge
        spawn the system.boot terminal (console.gui, namespace, shell kit,
            boot.doorbell) with cwd Sys:
        wait on boot.doorbell
        start the greeter
    terminal (system.boot):
        spawn the shell with argv[1] = Sys:S/Startup-Sequence, and pass
            boot.doorbell
    shell:
        run the command file
        signal boot.doorbell when its frame empties or EndCLI takes it

## What this is not yet

- **The failure view.** A sequence that fails leaves the shell standing, but
  today its window stays hidden rather than becoming the read-only view of the
  buffered output the failure arc will make it. That needs the boot log buffer
  (the terminal's scrollback, or one of its own) and a read-only state that
  takes no input.
- **Rescue mode.** A failed boot presents the output and no input by default;
  a kernel-command-line flag turns the same session into a full writable rescue
  shell. The flag's source is the board's bootloader — `bootargs` in the DTB
  `/chosen` node on the Orange Pi's u-boot; QEMU has no command line to read
  today. This is auth's and director's concern, not the shell's.
- **Headless operation.** The boot session assumes a console to give the
  terminal a window (even a hidden one). A server with no display is a later
  arc.

## Acceptance

The runner waits on `auth: the boot session ran` — auth's line after the boot
notification — so it proves both the system terminal and shell spawned and the
handshake fired, which only happens once `Sys:S/Startup-Sequence` has been read
and its frame drained. The boot window is hidden throughout, so it does not
disturb the acceptance's window pixel checks; the `NIL:` registration and the
redirection acceptance are `specs/shell.md`'s.
