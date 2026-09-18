# console: the display, the pointer, and the windows

Status: decided (2026-09), not yet implemented. The first slice is the
compositor, the focus model, and the greeter arc: auth's GUI login prompt,
and the desktop stub a successful login hands the screen to.

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
  desktop's backdrop is a full-screen window — rather than as a separate
  mechanism. Draggable screens, depth gadgets and the toolkit's look are
  later arcs; the greeter's look is deliberately basic, and what the toolkit
  will look like is its own spec.
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
- `create_window`. In: position and size in screen pixels, and the backing's
  offset within the caller's slice. Out: the window's id. Console
  clip-checks the backing against the slice: a window whose pixels would
  fall outside it is refused.
- `listen`. Out: one capability — the client's event notification (below).
  One channel per client; a second listen is refused.
- `damage`. In: the window's id and a rectangle in window-local pixels. The
  rectangle's pixels, as they stand in the slice, are composited to the
  screen: painter's algorithm, clipped against the windows above, then the
  gpu driver's `flush`. A tear-free flip is the driver's business when a
  device that has one arrives; compositing here is copy, not yet blend.
- `destroy_window`. In: the id. The window leaves the z-order and its
  damage is everyone else's redraw.
- `reap`. System authority's call — auth's, on session reclaim: every
  window the badge held is destroyed and its slice is free whole. This
  joins the reclaim order (`specs/auth.md`): reap the badge's windows and
  its handles, unbind its aliases, then revoke.

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
  delivered to the window under the cursor, and to the grab-held window for
  the length of a drag.
- **focus** — in and out. What a text field listens for.

## Focus and the pointer

Console owns the pointer: it tracks the position (the tablet's absolute
events are the natural feed; the mouse's relative ones integrate to the
same point), hit-tests the topmost window, and draws the cursor itself,
composited over the output — software for v1; virtio-gpu's hardware cursor
commands are noted, not taken.

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
accept is auth's cue to spawn the session as it does today, and the greeter
destroys its window and exits.

The session binary a successful login starts becomes the **desktop**: a
full-screen backdrop window — the Amiga screen, as a shape of window — with
`console.gui` in its `needs` where the smoke carried `devmgr.registry`.
What the desktop grows into is authority.md's later spec, unchanged.

## What this is not

A focus model beyond click-to-focus; window dragging, resizing, depth
gadgets, menus; alpha and blend; the hardware cursor; the second head
(gpu1 stays parked — multi-head is its own decision, about seats, not
windows); the toolkit. Each is easier with the compositor standing.

## Acceptance

Headless, from outside, the way the display checks already run: the
runner's QMP socket injects the keyboard events that type the known user's
name and secret (characters asserted through the keymap), injects the
tablet click on the button's coordinates, and reads the screen with
`screendump` — the greeter's window before login, the desktop's backdrop
after, and the focused window's frame where the click landed. The serial
`auth.login` test path stays: the port, not the pixels, is the credential
check.
