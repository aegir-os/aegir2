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
  bureau's Workbench menus. The Amiga shows the **active window's** menus in
  the screen bar, which needs a server an app registers with — the
  `bureau.menu` server, the next phase. This arc stands the bar and the
  interaction it needs; the server slots in behind the same bar.
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
as its content, gives it the Workbench menus, and runs `Application::exec`
instead of halting in `on_started`. An action logs a cue; the runner reads it.

## What this is not

The `bureau.menu` server (an app registering menus, shown when its window is
active); a menu's keyboard navigation; submenus beyond one level; desktop
icons and a launcher; the console reserving the screen-title strip so a
window cannot cover it; a window list. Each is its own step, and each is
easier with the bar standing.

## Acceptance

The login still starts the bureau, and the runner reads the screen: the
screen title bar's fill and a menu title where the bar stands, and the
Workbench grey where the backdrop does. It then clicks a menu title, reads
the dropped menu, clicks an item, and reads the cue the action prints. The
greeter and the demo are unaffected — both sit clear of the bar.
