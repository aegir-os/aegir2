# bureau: the session's desktop

Status: decided (2026-09). This is the spec the bureau arc lands under.
`specs/console.md` called the bureau "the Amiga screen, as a shape of window"
and left what it grows into to a later spec; this is that spec, and the arc
that makes the bureau a trinket client.

The bureau is the binary a login through the greeter starts
(`specs/auth.md`). The stub it has been is a raw grey fill with the screen
size hardcoded: it attaches its slice, paints 1280×800, and exits. The
greeter's arc (`specs/trinket.md`) made the toolkit real; this arc gives the
bureau the same base and the one protocol method a full-screen window needs.

## The decisions

- **The bureau speaks through the toolkit.** Like the greeter, it calls
  `Application::create`, sets the console port, and owns a `Window`. The
  raw console client it has been — its own allocator, attach, map, paint —
  is the greeter's pre-toolkit shape and goes the same way.
- **The screen's size is a query, not a constant.** `console.gui` gains
  `info` (`specs/console.md`'s "next method"): in nothing, out the width and
  height in pixels. A window whose rectangle falls off the screen is refused
  at `create_window`, so a stub with a hardcoded size is refused loudly the
  day the driver settles on another mode. The bureau asks, then covers.
- **The backdrop is one full-screen window.** `Window` in backdrop mode
  (`set_decorated(false)`, the console's `kWindowBackdrop`), its content a
  `Panel` filled with the theme's `BACKGROUND` — Workbench grey, the Amiga
  screen. It enters the z-order at the bottom and nothing raises it
  (`specs/console.md`'s focus model), which is what every Amiga user turned
  on anyway.
- **One shot, and the window stays.** The bureau paints, says so, signals and
  halts. The console owns the slice, so the window persists as the session's
  visible remainder without the process; the session's memory reclaims
  through the ordinary path (`specs/auth.md`). It halts rather than returning
  from `exec`, because `exec`'s teardown destroys the windows — the same
  reason the greeter does not return after its login.
- **The speculative bureau library is removed.** `libs/aegir-bureau`'s
  `wm::Client`, `menubar::Client` and `desktop::Backdrop` are stubs no caller
  reaches, and their protocol numbers were invented ahead of a server. They
  come back with the arcs that serve them, the way the toolkit's speculative
  pieces did (`specs/trinket.md`).

## The shape

### `console.gui`'s `info`

`kMethodInfo = 8`, after `listen`. The console answers `{width, height}` from
the mode its GPU driver settled on. `aegir::console::info(gui, &w, &h)` is the
client walk, the same shape as `attach`.

### The bureau

`Application::create` adopts the spawn kit and stands the heap up
(`specs/trinket.md`'s ordering). The bureau finds `console.gui`, sets it as
the application's port, asks its size, and builds one `Window` at
`{0, 0, width, height}` in backdrop mode. The content is a `Panel` whose
background is the theme's `BACKGROUND`. `show()` before `exec`, and
`Application::exec` creates and paints the window; the bureau's `on_started`
writes its cue, signals supervision, and halts — the login that started it
waits for that signal before the boot moves on (`apps/aegir-auth`).

The theme's `BACKGROUND` is the stub's own grey (`0x00AAAAAA`), so the screen
the acceptance reads is the same Workbench grey it read before; only the
process behind it changed.

## What this is not

Window decorations, title bars, dragging, resizing, depth gadgets and the
window manager; menus and the menu server; desktop icons and a launcher;
re-login — reaping a session's slice and starting the greeter again
(`specs/auth.md`); more than one window. Each arrives with the arc that
serves it, and each is easier with the bureau standing. The session's own
slice is deliberately not reaped (`specs/auth.md`): the backdrop is what the
session leaves on the screen.

## Acceptance

The login runs as it does now: the greeter's `auth.login` starts the bureau,
and `bureau: the screen is yours` is the cue. The runner reads the Workbench
grey where the bureau's backdrop stands — the same pixels the stub's
acceptance asserted (`specs/console.md`'s login arc), now proving the
toolkit's `BACKGROUND` covers the screen: a sample near each corner as well as
the centre, kept clear of the test bed's surviving window and the cursor. The
runtime's own proof and the flag-off build are unchanged and stay the
checkpoint.