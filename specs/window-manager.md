# window-manager: decorations, dragging, and depth

Status: decided (2026-09). This is the spec the window manager's first arc
lands under. `specs/console.md` kept the console a compositor with no policy —
windows overlap, focus is click-to-focus, and nothing raises — and left
"dragging, resizing, depth gadgets" to a later arc. `specs/bureau.md` gave the
bureau a backdrop and left the WM to this one. This is that arc, in its first
visible slice: **client-side titlebars and drag-to-move**.

The console composites rectangles; it has no idea what a titlebar is. So the
decoration is drawn by the window's own client, into its own backing, and the
console composites one rectangle as before. That keeps the console a
compositor and puts the look where the look lives (`specs/trinket.md`'s
theme). A server-side WM — the bureau drawing frames around other processes'
windows — would need the console to place frames it does not own, and is not
this arc.

## The decisions

- **Decorations are the client's.** A decorated `Window`'s console rectangle
  is the *frame*: a titlebar of the theme's `TITLEBAR_HEIGHT` above the
  content. `Window::rect()` stays the **content** rectangle — what a client
  sets and what layouts use — and the frame is derived: the console window
  sits at `(x, y - titlebar_height)` and is `(width, height + titlebar_height)`.
  The content keeps its screen position, so a form that was at (400,220) is
  still at (400,220) and only the titlebar appears above it.
- **The console gains `move` and `raise`.** Console still owns the z-order
  and the pointer; the client asks it to move and to raise, and console
  composites. This is the minimum the client cannot do for itself: it can
  paint a titlebar, but it cannot change where the console puts its window or
  what is on top of what.
  - `move`: in the window's id and its new (x, y), clip-checked against the
    screen. Console repaints the union of the old and new rectangles.
  - `raise`: bring the window to the top of the z-order. A backdrop cannot be
    raised (`specs/console.md`: the backdrop stays at the bottom). Console
    repaints the window's rectangle.
- **Focus stays click-to-focus and does not raise.** `specs/console.md`'s
  Amiga semantics: a button-down focuses, and does not raise. Raising is the
  client's explicit call, and the toolkit makes it on a titlebar-down — a
  deliberate act, not a side effect of focus.
- **Dragging is the toolkit's.** The console already grabs the pointer for the
  length of a button-down and delivers motion to the window holding the grab,
  window-local (`apps/aegir-console`). The toolkit begins a drag on a
  titlebar-down, raises, and on each motion computes the frame's new position
  and calls `move`; the up ends the drag. Nothing in the console changes for
  this — the grab and the motion are already there.
- **Resizing, depth gadgets, close/zoom gadgets, and the WM server are
  deferred.** A resize needs a grip and a console `resize`; depth gadgets need
  raise and lower as user acts; a `bureau.wm` server is policy the console
  does not need yet. Each arrives with the client that asks.

## The shape

### `console.gui`'s `move` and `raise`

`kMethodMove = 9` (id, x, y) and `kMethodRaise = 10` (id), after `info`.
`aegir::console::move(gui, id, x, y)` and `aegir::console::raise(gui, id)` are
the client walks. A move whose frame would fall off the screen is refused
(the same clip-check `create_window` makes); a raise of a backdrop is
refused.

### The toolkit's decorated `Window`

`Window` with `decorated_` (the default) draws its frame into the backing:
the theme's `TITLEBAR_BG`/`TITLEBAR_TEXT` strip at the top, the title text
from `set_title` (the active colour while focused), and the theme's
`draw_window_frame` around the whole rectangle. The content is painted below
the titlebar: the content root's rectangle is `{0, titlebar_height, width,
height}` in frame-local coordinates, so every widget rect stays frame-local
and the hit test descends unchanged.

`backing_bytes()` is the frame's: `width * (height + titlebar_height) * 4`. A
client sets its content rectangle as before; `set_rect` moves the frame,
fires `on_moved_resized`, and repaints. `set_title` repaints.

A pointer-down in the titlebar begins a drag and raises; motion moves the
window through `console::move`; the up ends it. A pointer-down in the content
is the widgets', as before.

### The clients

The greeter's window gains its titlebar and keeps its form where it was: a
`set_title` and nothing else. The bureau's backdrop is `set_decorated(false)`
and so has no titlebar — the Amiga screen is not a window with a frame.

## What this is not

Resizing and resize grips; depth gadgets and explicit lower; close, zoom and
roll-up gadgets; a window list or TaskX; the `bureau.wm` server and the
`bureau.menu` server (`specs/trinket.md`'s `MenuBar`); themed decorations
beyond the XEN titlebar the theme already carries. Each is its own arc.

## Acceptance

The greeter's form is at the same screen coordinates as before — the titlebar
is above it — so the form's pixels are unchanged, and the titlebar is the new
check: its background (`TITLEBAR_BG`) and its title text where the titlebar
stands. The client-side frame is what those pixels prove: the toolkit drew a
titlebar into its own backing and derived the frame the console composites.

The move, raise and drag are read in the source and exercised at the keyboard
(`make run-ui`): the automated drag lands with the WM's own test client, and
deliberately not here, because a drag moves the very window the login is
paced against and would make this arc's acceptance depend on the next one's.
The console's grab and window-local motion, which the drag rides on, are the
console's and exercised since its arc.