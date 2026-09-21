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
  deliberate act, not a side effect of focus. A client that wants to *start*
  focused asks: the console gains `focus` (no raise), and the toolkit's
  `Window::request_focus` sends it, kept until the window exists. The
  greeter asks, so its form is ready to type into without a click; the
  bureau's backdrop does not, because the screen is not a thing to focus.
- **Dragging is the toolkit's.** The console already grabs the pointer for the
  length of a button-down and delivers motion to the window holding the grab,
  frame-local for an ordinary window but in **screen coordinates for the grab
  holder** (`apps/aegir-console`): a window the drag is itself moving has no
  stable frame, so an event queued while it was elsewhere would read against
  the wrong origin — the "thrown" drag. The toolkit begins a drag on a
  titlebar-down, raises, and on each motion computes the frame's new position
  from the pointer's screen position and the offset where the drag began; the
  up ends the drag. Nothing in the console's grab changes for this — only the
  coordinates it carries.
- **Resizing is a grip and a `resize`.** A decorated window carries a grip
  in its bottom-right corner; a drag there changes the window's size, and the
  console gains `resize` (in the id and the new width and height,
  clip-checked like `move`, the old and new rectangles composited). The
  content keeps its coordinate space: the grip is frame-local, the resize
  changes the frame, and the window re-lays-out its content at the new size.
  - **The backing is reserved at the screen-bounded maximum.** `attach`
    carves one slice per badge and a second is refused, so the slice cannot
    grow at run time. `Application` already sizes it from the visible
    windows; a resizable window therefore asks for its *maximum* backing —
    the screen's size, the largest frame the console will accept — and a
    resize stays inside it. It is arena memory spent for headroom, and the
    alternative (a resize that outgrows its backing) is a window that cannot
    grow.
- **A repaint and a flush carry the rectangle that changed.** The toolkit
  unions an event's damages, clips its paint to the union, and hands that
  rectangle to `console::damage`; the console composites it and the driver
  transfers and flushes only it (`specs/console.md`, `aegir/framebuffer.h`).
  The whole-window repaint on every keystroke, and the whole-screen transfer
  on every event, were the lag a real client found first; a window's slice is
  resolved once at create, not per pixel.
- **A move shifts; it does not recomposite.** Moving a window still changes
  every pixel of the union, but where the moved window was and is the
  topmost, the new screen there is the old screen shifted by the move: the
  console memmoves the overlap and recomposites only the strips the move
  revealed and uncovered. Compositing the whole window per motion was what
  left a drag trailing the cursor, which moves only its 8×8. A move of a
  window that is not topmost falls back to the union composite.
- **The titlebar carries three gadgets.** Packed at its right: close, zoom
  and depth. Close calls the client's `on_close_requested` and hides the
  window; zoom toggles between where the window was and the whole screen,
  its titlebar at the top; depth lowers the window to the bottom among the
  plain windows (the console gains `lower`). The titlebar elsewhere still
  raises: a drag or a resize brings the window forward, the gadgets act
  deliberately. Depth is on by default; a client asks for close and zoom
  (`set_gadgets`), because a login window should not be closable and the
  bureau's backdrop has no titlebar at all. A zoom is a move and a resize in
  the order that keeps the intermediate state on the screen — shrink before
  moving, grow after.
- **The WM server is deferred.** A `bureau.wm` server is policy the console
  does not need yet; it arrives with the client that asks.

## The shape

### `console.gui`'s `move`, `raise` and `resize`

`kMethodMove = 9` (id, x, y) and `kMethodRaise = 10` (id), after `info`;
`kMethodResize = 11` (id, width, height), `kMethodLower = 12` (id) and
`kMethodFocus = 13` (id), after them. `aegir::console::move`, `raise`,
`resize`, `lower` and `focus` are the client walks. A move or a resize whose
frame would fall off the screen, or a resize past the client's slice, is
refused (the same clip-check `create_window` makes); a raise, lower or focus
of a backdrop is refused.

### The toolkit's decorated `Window`

`Window` with `decorated_` (the default) draws its frame into the backing:
the theme's `TITLEBAR_BG`/`TITLEBAR_TEXT` strip at the top, the title text
from `set_title` (the active colour while focused), and the theme's
`draw_window_frame` around the whole rectangle. The content is painted
through a canvas offset by the titlebar, so the content keeps its own
coordinate space: the content root's rectangle is `{0, 0, width, height}`
and the widgets never hear about the titlebar. Pointer coordinates are
translated the same way before the hit test.

`backing_bytes()` is the screen-bounded maximum of the frame — the largest a
resize may reach. A client sets its content rectangle as before; `set_rect`
moves the frame, fires `on_moved_resized`, and repaints. `set_title`
repaints.

A pointer-down in the titlebar begins a drag and raises; motion moves the
window through `console::move`. A pointer-down in the bottom-right grip
begins a resize; motion resizes through `console::resize`, bounded by
`min_size_` and the screen. A pointer-down on a titlebar gadget — close,
zoom or depth, at the right — acts: `close()` (the client's
`on_close_requested`), `zoom()` (the toggle above), or `console::lower`.
The up ends either gesture. A pointer-down anywhere else in the content is
the widgets', as before.

### The clients

The greeter's window gains its titlebar and keeps its form where it was: a
`set_title` and nothing else. The bureau's backdrop is `set_decorated(false)`
and so has no titlebar — the Amiga screen is not a window with a frame. The
demo client (`apps/aegir-gui-demo`) is a decorated window with all three
gadgets, clear of the others, that the runner clicks.

## What this is not

Roll-up gadgets; a window list or TaskX; the `bureau.wm` server and the
`bureau.menu` server (`specs/trinket.md`'s `MenuBar`); themed decorations
beyond the XEN titlebar and gadgets the theme carries. Each is its own arc.

## Acceptance

The greeter's form is at the same screen coordinates as before — the titlebar
is above it — so the form's pixels are unchanged, and the titlebar is the new
check: its background (`TITLEBAR_BG`) and its title text where the titlebar
stands. The client-side frame is what those pixels prove: the toolkit drew a
titlebar into its own backing and derived the frame the console composites.

The test bed exercises `move`, `resize` and depth end to end, where the
console's window protocol already lives: the red window to the top-left, read
back there with the backdrop repainted where it stood, then resized, read back
at its new rectangle with the uncovered strip repainted, and then the white
raised over it and lowered beneath it, the overlap reading white while it is
up and red once it is down.

The demo client exercises zoom and close: the runner clicks its zoom gadget
and reads the window filling the screen, titlebar at the top and active;
clicks zoom again and reads it back; then clicks close and reads the bureau's
backdrop where it stood. It runs last, after the login and the bureau, so its
clicks never race the login's.

The drag and the grip are read in the source and exercised at the keyboard
(`make run-ui`): their automated test lands with the WM's own test client,
and deliberately not here, because a drag moves the very window the login is
paced against and would make this arc's acceptance depend on the next one's.
The console's grab and window-local motion, which both gestures ride on, are
the console's and exercised since its arc.