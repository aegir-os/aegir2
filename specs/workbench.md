# workbench: the screen, its title bar, and its menus

Status: decided (2026-09). This is the spec the Workbench arc lands under.
`specs/bureau.md` gave the bureau a backdrop and left the desktop to a later
arc; `specs/amiga-fidelity.md` records the menu deviation. This is that arc,
in its first slice: **a live bureau with a screen title bar and an
always-visible menu bar**.

On the Amiga the Workbench is not a program in a window; it is the **screen**:
a title bar across the top carrying the screen's name and its menus, and the
desktop beneath. Windows sit on it and are reaped back to it. Aegir already
has the shape — the bureau's backdrop is the screen (`specs/bureau.md`) — but
it painted once and exited. This arc makes it live and gives it the bar.

## The decisions

- **The bureau lives.** It no longer paints and halts: it runs its event loop,
  owns the screen title bar and the menus, and answers the pointer. The
  console still owns the slice, so the backdrop stands whether or not the
  bureau is scheduled; a live bureau is what can act on a menu. It does **not**
  signal its supervision as a ready: auth reads a session's first supervision
  as its exit and reclaims the session (`apps/aegir-auth`), which a
  short-lived smoke can be and the desktop cannot. auth therefore waits for
  the exit this never sends, which is what a session that is the desktop
  means; the ready/exit split is the supervisor arc's.
- **The screen title bar is the bureau's, drawn into the backdrop.** It is a
  bar across the top of the screen holding the screen's menus. Because it is
  the backdrop's own top, a window can be dragged over it — the Amiga keeps
  the screen bar above the windows, and reserving the strip is the console's
  (a later arc, noted below). For now the greeter and the demo sit clear of
  it, and a zoomed window covers it as it covers the screen.
- **Menus are always visible, not on the right mouse button.** Workbench shows
  a screen's menus in the title bar only while the right button is held; Aegir
  draws them always, because a right-button drag is awkward on a modern
  laptop's touchpad. This is the deliberate deviation `specs/amiga-fidelity.md`
  records.
- **The menus are the bureau's own, for now.** The screen bar shows the
  bureau's own menus. The Amiga shows the **active window's** menus in
  the screen bar, which needs a server an app registers with — the
  `bureau.menu` server, the next phase. This arc stands the bar and the
  interaction it needs; the server slots in behind the same bar, and its
  design is the section below.
- **A click opens, a click acts.** Clicking a menu's title drops its menu
  below the bar; clicking an item runs it and closes the menu. Clicking
  elsewhere, or the title again, closes it. (Workbench's press-drag-release is
  the right-button model this deviation leaves behind.)

## The shape

### The `Desktop`

`libs/aegir-bureau`'s `Desktop` is a trinket `Widget` whose rectangle is the
whole screen. Its `on_paint` fills the backdrop with the theme's `BACKGROUND`,
draws the screen title bar (a beveled bar, `specs/amiga-fidelity.md`'s
title-bar look) and its menu titles, and, when a menu is open, its items below
the bar. Its pointer handlers open, close and act on the menus; `on_action`
is the client's. A widget rather than a bare canvas because its rectangle is
the screen, so the open menu is inside it and nothing clips it.

The menu model is the toolkit's `MenuBar::Menu` — titles and items, an item's
`action_id`, its flags — so the `bureau.menu` server serializes the same shape
later.

### The bureau

`apps/aegir-bureau` builds the backdrop `Window` as before, sets a `Desktop`
as its content, gives it the bureau's menus, and runs `Application::exec`
instead of halting in `on_started`. An action logs a cue; the runner reads it.

## The `bureau.menu` server (the next phase)

The bar stands; this is the server that slots in behind it. The Amiga shows
the **active window's** menus in the screen bar, so an app registers its menus
and the bureau draws them while that app's window is active. Six decisions
make it hold.

- **The bureau owns `bureau.menu`, the first port a session owns.** It is a
  port like any other in the graph — declared `owns` on `[session.bureau]`,
  its endpoint made by the director, its caller halves handed out by the
  manifest's `needs` — but a session is not the director's to spawn, so the
  owner half cannot travel the way a boot service's does. It travels the way a
  session's other ports do: through auth. auth's spawn kit already carries an
  unbadged copy of every port its covered children *need*; it grows the
  symmetric rule, an unbadged **owner** copy of every port a covered child
  *owns*, named `spawn:bureau.menu` as the others are named `spawn:...`. auth
  passes that half to the bureau at login, beside the console and the
  namespace. Nothing else changes: the director still makes the endpoint, and
  a client's half is a `needs` like any other. The port's caller half carries
  Grant, because `menu_register` transfers a capability (the namespace's
  reason, `apps/aegir-director/src/ports.cc`).

- **The bureau serves it with the console's one receive.** A single thread
  cannot both `seL4_Wait` on the console's event notification and `seL4_Recv`
  on a port, so it does what the console does: bind the notification to its own
  TCB and `seL4_Recv` on the port. A delivery the kernel carries no message on
  is the notification — drain the event ring; a call is the protocol
  (`apps/aegir-console/src/main.cc`). `Application` grows an optional served
  port and takes that shape when one is set; an app that serves none keeps
  `seL4_Wait`.

- **Focus is the client's to report.** The console sends a focus event only to
  a window's owner, so the client — which owns its window — is the one that
  knows. On `on_focus_gained` it calls `menu_set_active`; on `on_focus_lost` it
  clears. The bureau shows that client's menus in place of its own, and its own
  when none is active. No console change: the routing the console already does
  is what tells the client.

- **Registration is one call, and the doorbell rides with it.** `menu_register`
  carries the menu tree in the call's words and, by capability transfer, a
  signal-only copy of the notification the client already listens on — the
  console's doorbell. The console's listen mint therefore carries Write beside
  Read: a mint can only keep rights its source holds, and the client must mint
  the signal copy the bureau rings. The signal is the client's own doorbell, so
  waking itself is all the widened right buys. The bureau keeps the tree and the
  cap. A tree larger than the kernel's message registers is refused; that ceiling
  is the envelope's (`kMaxWords`), not a number chosen here, and a shared-window
  form is the escape hatch when a real tree needs one.

- **An action rings the client's doorbell and is fetched.** The bureau does not
  call into the client or hold a call open: a single-threaded client cannot be
  mid-call and in its event loop at once. It stores the action and signals the
  doorbell; the client, already woken by it, calls `menu_take_action` to fetch.
  The client's loop is unchanged but for a poll, which the toolkit offers as an
  `on_poll` hook after a drain.

- **The wire is the toolkit's own model.** The model is `MenuBar::Menu`, so the
  Desktop draws what it already draws. Strings are `std::u32string`'s code
  points, two to a word; a menu is a count and its title, an item its action id,
  flags and label. The methods are `menu_register`, `menu_set_active`,
  `menu_take_action`. The protocol lives in `libs/aegir-bureau`
  (`aegir/bureau/menu.h`), the shape of every other port's
  (`libs/aegir-console`'s is the pattern).

The `Desktop` gains `set_client_menus` and `clear_client_menus` and an
`on_client_action`; the bureau forwards a client action to the active client's
doorbell. The registry — the clients, their trees, their doorbells, the active
one — is the bureau's, and the desktop it draws into is the same desktop the
bar already is.

The console owning each client's event notification is what makes the listen
mint's Write right necessary. The better end state is the **client** owning its
own notification and handing the console a signal-only cap to it: the console
then holds less than it does today, and the client mints the bureau's copy from
an object that is its own. That is a change to `listen` and to every client —
the greeter, the demo, the bureau, the test bed — so it is its own arc, recorded
here as where the event channel should end up.

## What this is not

A menu's keyboard navigation; submenus beyond one level; desktop icons and a
launcher; the console reserving the screen-title strip so a window cannot cover
it; a window list. Each is its own step, and each is easier with the bar
standing.

## Acceptance

The login still starts the bureau, and the runner reads the screen: the
screen title bar's fill and a menu title where the bar stands, and the
Workbench grey where the backdrop does. It then clicks a menu title, reads
the dropped menu, clicks an item, and reads the cue the action prints. The
greeter and the demo are unaffected — both sit clear of the bar.

The `bureau.menu` server's acceptance is the demo's, because the demo is the
first client: the runner clicks the demo's window, the demo reports the focus,
and the runner reads the **demo's** menu titles in the bar where the bureau's
stood; it clicks one, and the demo prints the action's cue — proof the tree
crossed to the bureau and the action crossed back. Clicking the backdrop
restores the bureau's own titles. The greeter still shows none: it never
registers, so the bar is the bureau's while it is active.
