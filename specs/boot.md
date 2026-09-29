# boot: Startup-Sequence, and the boot session

Status: the boot session's success and failure paths are implemented (2026-09).
This spec is the boot arc's — the system's startup command file, the session
that runs it, the read-only view a failed boot leaves, and the boot flag that
forces one. The rescue shell the flag will also select is the next slice.
`specs/shell.md` is the command line that runs it; this file is the boot's own
shape.

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
- **The shell reports the outcome, and auth waits.** The boot terminal is
  granted an endpoint (`boot.status`) it passes to its shell; the shell sends
  one word when the command file's frame empties or `EndCLI` takes it — 0 when
  Startup-Sequence finished, nonzero when a command failed at or above the fail
  level or the firmware forced a failure. `auth` receives it and starts the
  greeter only on success. A script that never ends would hold the boot, which
  is the same wait a service's ready is.
- **The quiet default is `EndCLI >NIL:`.** `NIL:` is a volume
  (`specs/vfs.md`), so the sequence's close is silent with no special case, and
  the boot window never appears unless the script writes.
- **The boot session's window is hidden until the sequence fails.** A quiet
  sequence leaves no window, and `>NIL:` keeps it that way. When the sequence
  fails, the shell reports the outcome and asks the terminal for the read-only
  failure view: the terminal shows its window — its backing was reserved at
  start, because the toolkit sizes an app's slice from the windows shown or
  reserved then — and takes no more input, so the grid holds what the sequence
  wrote and nothing else. On success the script's `EndCLI >NIL:` closes the
  window and auth starts the greeter; on failure the view stands and no greeter
  runs.
- **The boot session has a launcher, so its sequence runs commands.** A command
  in the sequence resolves through `C:` (`Sys:C`, `specs/dos.md`), which auth
  binds for the boot badge, and starts through the boot session's
  `launch.session` -- the same launcher a login's shell uses (`specs/launch.md`),
  which auth starts beside the terminal. Its namespace copy is badged for the
  boot badge, so the launcher and its commands resolve the boot session's `Sys:`
  and `C:`, and its commands take system serials (a system badge has no range).
  `Sys:` is a filesystem's to register and comes up after
  auth, so the bind is retried until the boot volume is there — the same wait
  the user database's resolve does.
- **The firmware's boot flags are the device tree's `/chosen/bootargs`.** The
  bootloader's command line is where a boot's mode is named — `aegir.fail`
  forces the failure view, and the rescue shell the next slice adds will be
  `aegir.rescue`. Director reads the one property from the device tree it
  already owns and hands the string to auth through the bootstrap block's
  `Boot` entry; auth is the service that starts the boot session and decides
  what a failed boot does. Auth passes `aegir.fail` on as the boot terminal's
  environment (`AEGIR_BOOTARGS`), which its shell reads and obeys by reporting a
  failure even though the sequence succeeded — so the failure path is reachable
  on a machine whose sequence is fine. The elfloader is told to use the
  bootloader's tree rather than one baked into its CPIO (`ElfloaderIncludeDtb`
  off): the baked-in tree carries no `bootargs`, and the option's own comment —
  "in case bootloader doesn't provide one" — is not what its code does.

## The boot session

    auth:
        read the database
        wait for Sys:C, bind C: for the boot badge
        spawn the system.boot terminal (console.gui, namespace, shell kit,
            boot.status, AEGIR_BOOTARGS) with cwd Sys:
        spawn the system.launcher that serves launch.session, and hand the
            terminal its caller half
        receive the outcome on boot.status: 0 runs the greeter, nonzero does not
    terminal (system.boot):
        spawn the shell with argv[1] = Sys:S/Startup-Sequence, and pass
            boot.status and the launcher's caller half
    shell:
        run the command file, starting each command through launch.session;
            on failure (or aegir.fail) ask the terminal for the read-only
            failure view
        send the outcome on boot.status when its frame empties or EndCLI takes it

## What this is not yet

- **Rescue mode.** A failed boot presents the output and no input by default;
  the flag turns the same session into a full writable rescue shell. The flag's
  source is plumbed — director reads `/chosen/bootargs` and hands it to auth
  (`Boot`, above), and auth passes `aegir.fail` to the shell — but the writable
  mode itself is the piece after this one. This is auth's and director's
  concern, not the shell's.
- **Headless operation.** The boot session assumes a console to give the
  terminal a window (even a hidden one). A server with no display is a later
  arc.

## Acceptance

The runner waits on `auth: the boot session ran` — auth's line after the boot
outcome — so it proves both the system terminal and shell spawned and the
handshake fired, which only happens once `Sys:S/Startup-Sequence` has been read
and its frame drained. The boot window is hidden throughout, so it does not
disturb the acceptance's window pixel checks; the `NIL:` registration and the
redirection acceptance are `specs/shell.md`'s.

The failure path is a second target, `aegir-fail` (`make run
TARGET=aegir-fail`): the same machine booted with `-append aegir.fail`, so auth
passes `aegir.fail` to the boot shell, which reports a failure although
Startup-Sequence succeeds. The runner waits on `terminal: boot failed, the view
is up` — the marker for this target, because the director's boot wait is paced
by the test bed and no greeter runs — and on `auth: the boot session failed --
the failure view stands`, and a screendump of the console's head shows the boot
window up, its grid grey where the session terminal's would be, the blue
backdrop still around it. Both cues together prove the shell reported a
failure, auth left the view standing instead of starting the greeter, and the
window came up.
