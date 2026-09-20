# console: the display, the pointer, and the windows

Status: landed (2026-09): the compositor, the focus model, the event
channel, and the greeter arc — auth's GUI login prompt, and the bureau
stub a successful login hands the screen to.

`console` is a system service in the boot set (`specs/services.md`), started
by director, owning one port — `console.gui` — and needing `log.main` and
`devmgr.registry`. It is the only process that touches the display and the
input devices: it opens `gpu.virtio0`, `kbd.virtio0`, `mouse.virtio0` and
`tablet.virtio0` through the registry at boot, and what every other process
sees of the screen and the keyboard is what console serves.

## The decisions

- **Users do not hold device capabilities.** This answers authority.md's
  open question, for display and input: no. The gpu and the HID devices are
  console's alone; a session holds windows and event channels, and the
  device manager's registry stays a system service's tool. The smoke
  session's direct `tablet.virtio0` open (`specs/auth.md`'s input path) is
  the interim this replaces — recorded sufficient while sessions were
  smokes, and superseded here.
- **Windows from day one.** Console is a compositor, not a screen switcher:
  clients create overlapping windows, console owns the z-order, and focus is
  hit-tested (below). The Amiga screen survives as a shape of window — the
  bureau's backdrop is a full-screen window — rather than as a separate
  mechanism. Draggable screens, depth gadgets and the toolkit's look are
  later arcs; the greeter's look was deliberately basic here, and the toolkit
  that replaced it — its console integration, its XEN look, its embedded font
  — is `specs/trinket.md`.
- **Pixels never cross a message.** The window protocol is the established
  shared-window shape (`specs/services.md`: bulk data never crosses the
  message), turned around: console carves one **arena** per client out of
  its own delegation, megapage-sliced, and the client maps the slice and
  draws into it. Console mapped the whole arena when it carved it, so
  compositing is console reading memory it already has. A window's backing
  is an offset into the client's slice — chosen by the client, which
  allocates its own slice — so creating and destroying windows is
  bookkeeping, not memory traffic.
- **Capabilities ride this protocol.** Attaching hands over the slice's
  frame caps — one cap per reply, the registry-`open` precedent — and
  `listen` hands over the client's event notification. This is services.md's
  "may ports carry capabilities?" answered as it predicted: individual
  ports, a protocol that says it carries them. Console's is the first.

## The `console.gui` protocol

Owned by console. Strings, when they arrive, travel in the namespace
protocol's shape; the method numbers below are the version rule, and a
method console does not know is answered by saying nothing.

- `attach`. The introduction: console badges the caller's state, carves the
  slice, and answers with the slice's geometry — base offset, bytes, frame
  count, frame bits. The frames themselves come one per `frame` call, and
  the client maps them into its own address window with its own VSpace
  root, the way every service already maps what it is given.
- `frame`. In: the index within the slice. Out: that frame's capability.
- `create_window`. In: position and size in screen pixels, the backing's
  offset within the caller's slice, and flags. Out: the window's id. Console
  clip-checks the backing against the slice: a window whose pixels would
  fall outside it is refused. The one flag is `backdrop`: the window enters
  the z-order at the bottom and, with no raise in the focus model, stays
  there — the bureau's shape.
- `listen`. Out: one capability — the client's event notification (below).
  One channel per client; a second listen is refused.
- `info`. In: nothing. Out: the screen's width and height in pixels — the
  mode the driver settled on. A full-screen window must match it, and a
  client that guesses is refused at `create_window` (`specs/bureau.md`).
- `damage`. In: the window's id and a rectangle in window-local pixels. The
  rectangle's pixels, as they stand in the slice, are composited to the
  screen: painter's algorithm, clipped against the windows above, then the
  gpu driver's `flush` of that rectangle — the whole screen per event was
  the lag that partial damage and a region flush removed. The first damage
  is also what *shows* a window:
  before it the rectangle is composited as if the window were not there —
  the backing is the client's to paint first, and retyped frames arrive
  dirty. A tear-free flip is the driver's business when a device that has
  one arrives; compositing here is copy, not yet blend.
- `destroy_window`. In: the id. The window leaves the z-order and its
  damage is everyone else's redraw.
- `reap`. System authority's call — auth's, when a badge's windows must go:
  every window the badge held is destroyed and its slice is free whole.
  Today's caller is the login arc (the greeter, reaped before its login's
  session starts); a session's own slice is the re-login arc's to take down
  (`specs/auth.md`).

## The event channel

One channel per client: a notification (console signals, the client waits)
and a ring buffer in the slice's last 4 KiB page — console mapped the whole
slice when it carved it, so appending is writing memory it already has, and
the client maps the page with the rest. The endpoint shape this replaces —
an endpoint per window, serving poll/next — does not survive the kernel's
one-blocked-receive rule on either side: console cannot receive on the gui
port and every window's endpoint, and a client thread waits on one
notification where it could not wait on many endpoints. Events are tagged
with their window's id instead. The envelope is the input driver's packed
word (`libs/aegir-input`), and a full ring drops — console never blocks on
a client that stopped reading.

Console's own side of the devices is the same bound-notification shape the
drivers wait on their interrupts with: the input protocol's `subscribe`
hands each driver a minted, badged notification, a signal wakes the receive
that serves the port, and console drains with `poll`/`next` — the queue is
the driver's, so coalesced signals lose nothing.

The vocabulary is console's (`libs/aegir-console`), three kinds to start:

- **key** — the raw code, and the translated character in the same event.
  The keymap is console's (a table in the service, US layout v1): key-code
  to character is a property of the system's HID layer, not of every client
  that takes text. Unmapped codes carry a zero character.
- **pointer** — motion and buttons, in **window-local coordinates**,
  delivered to the window under the cursor; and, for the length of a drag, to
  the grab-held window in **screen coordinates**, because a window the drag
  is moving has no stable frame to be local to (an event queued while the
  window was elsewhere reads against the wrong origin, which is what an
  overshooting "thrown" drag was).
- **focus** — in and out. What a text field listens for.

## Focus and the pointer

Console owns the pointer: it tracks the position (the tablet's absolute
events are the natural feed; the mouse's relative ones integrate to the
same point), hit-tests the topmost window, and draws the cursor itself,
composited over the output — software for v1; virtio-gpu's hardware cursor
commands are noted, not taken. The cursor is a 16×16 arrow: white, with a
black border one pixel around it and a dark shadow one pixel down and right,
so a white pointer stays visible on a white surface.

Focus is **click-to-focus**, Amiga semantics: a button-down inside a window
makes it the focus, and does not raise it — depth arrangement arrives with
the gadgets that ask for it. Keyboard events go to the focused window's
channel. Before any client attaches, focus is nothing and the events go
nowhere — the boot's early key presses are nobody's business, as they were
for the test bed's cue ordering (`specs/services.md`).

## The login arc

The greeter is auth's face, and auth spawns it — the established pattern:
the service that knows, starts it. Auth's `needs` gain `console.gui`, the
director hands auth the `spawn:`-prefixed copy, and after its database is
read auth starts the greeter with the minted ports. The greeter is a pure
UI process: one window, two text fields, a button, an error line. It calls
`auth.login` over the existing port — console is not a login caller, and
the credential check never leaves auth. A refuse redraws the error line; an
accept is the greeter's cue to welcome the user and exit, and auth's —
waiting on that exit, so the welcome is written before the teardown — to
reap the greeter's badge (window and slice, the session reclaim's order)
and start the session.

The session binary a login through the greeter starts is the **bureau**: a
full-screen window, always in backdrop mode — the Amiga screen, as a shape
of window — in Workbench grey, with `console.gui` in its `needs`. It draws
and exits: the console owns the slice, so the window persists as the
session's visible remainder, and reaping it is the re-login arc's.
What the bureau is, and what it grows into, is `specs/bureau.md`.

## What this is not

A focus model beyond click-to-focus; window resizing, depth gadgets, menus;
alpha and blend; the hardware cursor; the second head (gpu1 stays parked —
multi-head is its own decision, about seats, not windows); the toolkit.
Window dragging and depth arrived with the window manager
(`specs/window-manager.md`): the console gained `move` and `raise`, and the
client draws its own titlebar. Each is easier with the compositor standing.

## Acceptance

Headless, from outside, the way the display checks already run: the
runner's QMP socket reads the greeter's form with `screendump` — and the
form stands through the test bed, the login played after the boot marker —
then injects the motion and click that focus the window, and, paced on the
greeter's focus line (the click and the keys ride different queues, and
which drains first is the boot's timing, not the script's), the keys that
type the known user's name and secret (characters asserted through the
keymap). The bureau's Workbench grey is read back where the form stood, the
samples kept clear of the test bed's surviving window and of the cursor.
The serial `auth.login` test path stays: the port, not the pixels, is the
credential check.
