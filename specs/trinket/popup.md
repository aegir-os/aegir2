# trinket/popup: the popup layer

Status: decided (2026-09). The piece the cycle's menu and the popup button wait
for (`specs/trinket/cycle.md`, `specs/trinket/popup_button.md`), under
`specs/trinket/`.

## The problem

Both gadgets are shapes without a consequence: the cycle advances but never
shows its entries, and the popup button reports a click that opens nothing. What
they want is a list drawn **above** the window's content, taking the pointer
while it is up -- MUI's popup.

A popup cannot be a widget in the content tree. `Window::paint` walks the tree
in order and hands the console one damaged region, so a popup owned by the cycle
would be painted *under* every sibling that follows it -- the toggles, the
label, the list below. It is the window's own layer instead.

## The decisions

- **The window owns one popup.** `Window::open_popup(std::unique_ptr<Widget>
  content, Rect rect)` takes the widget, in *window-content* coordinates, and
  `close_popup()` gives it up. One at a time: MUI stacks them, and nothing here
  wants a stack yet.

- **It is painted after the content, inside the content's area, and opening it
  damages its own rectangle.** `paint()` runs the content tree, then the popup
  through the same offset canvas, in the same damage region. That region is the
  one an *event* damaged, so the click that opens a popup damages only the widget
  it landed on, and a popup painted into that region alone comes up with
  everything outside that widget missing -- its foot, the first time this was
  tried. Opening damages the popup's whole rectangle for this reason. It is
  clipped to the content's rectangle -- a popup that overflows the window is a
  second console window, which is its own arc -- and it takes no titlebar or
  frame of its own. The console composites per window, so the popup cannot leave
  the window and does not need to.

- **A close asked for inside a dispatch waits for it to return.** A menu's own
  list is what picks an entry: `ListView::select` calls the client's `on_select`
  from inside `on_mouse_down`, and the client's handler is what closes the popup
  -- so resetting the popup there frees the widget whose handler is still on the
  stack. The window defers the reset to the end of the dispatch, as it already
  defers a repaint asked for from inside a paint. `open_popup` replacing a popup
  does not wait, because the widget asking is not the popup's.

- **A cue names exactly one event.** The cycle's menu and the popup button's
  object are both this window's one popup layer and both report through its one
  callback, so the cue has to say *which*: the opener knows, and the demo prints
  `demo: menu N` for the cycle's menu and `demo: object N` for the popup button's
  object. The acceptance answers a cue with the steps that watch for it, and a
  cue two events shared fired the object's pick on the cycle's menu -- the object
  was then never picked and the run waited for a cue that could not come.

- **The pointer goes to it first.** With a popup up, a pointer down inside it
  goes to the widget under the point; a pointer down *outside* dismisses the
  popup and is swallowed -- MUI's rule, and what makes a menu feel like a menu
  rather than a thing the click falls through. Motion inside goes to the widget
  under the point, so the list's cursor follows as it always does.

- **The keyboard goes to it too.** Opening focuses the popup's content (the
  window's own focus is saved), Escape dismisses, and every other key goes to
  the popup's focused widget -- so a menu's arrows work without the client
  wiring them. Closing restores the focus the window had.

- **Its damage is the window's.** The popup's content is a widget with the
  window set, so a damage inside it repaints the window through the same path a
  content widget's does; there is no second damage channel to forget.

- **The frame is the client's, and a menu's is its list's own.** The layer takes
  *any* widget as the popup root, so what a popup looks like is the client's
  choice from the theme's parts. A menu's root is the list itself: the list's
  well is one border, and wrapping it in a frame -- the obvious first move -- gives
  a menu two, which is not what the MUI screenshot shows.

- **The anchor is a rule, not a guess.** `popup_rect(anchor, popup, bounds)`
  places the popup below the widget that opened it, above it when it does not
  fit below, and inside `bounds` either way. It is pure, so `make check-popup`
  pins it -- the cases being exactly the ones a screen shows only by luck: a
  popup that fits below, one that must go above, one taller than the bounds, and
  one wider.

- **The cycle uses it for its entries.** MUI's split, now that there is a popup:
  a click on the cycle's **button cell** advances, and a click on its **text**
  opens a list of the entries. It hangs **under that text, just past the button
  cell, and is as wide as that text area** -- anchored at the box's left edge it
  would sit under the mark rather than under the text it belongs to. Its entries
  read centred, as a menu's do. Picking one sets the active entry and reports it,
  as the keys already do; the menu closes either way.

## The shape

```cpp
// popup.h
Rect popup_rect(Rect anchor, Size popup, Rect bounds);

// window.h
void open_popup(std::unique_ptr<Widget> content, Rect rect);
void close_popup();
bool has_popup() const;
Widget* popup() const;
```

## What this is not

- **A stack of popups, or sub-menus.** One at a time; a menu that opens another
  menu waits for a client that wants it.
- **A shadow, a fade, or alpha.** XEN popups are opaque; the toolkit has no
  blend beyond the canvas's per-pixel path.
- **A popup outside the window.** Its own console window is the arc that would
  give one, and the requester is the client that would want it.
- **Drag-and-drop.** MUI's popups double as drop targets; nothing here drags.

## Acceptance

- **`make check-popup`** (`scripts/check_popup.py` + `scripts/popup_conformance.cc`):
  the anchor rule, including the flip above and the clamps to the bounds.
- **The demo** opens the cycle's menu with a click on its text, picks an entry,
  reads `demo: cycle N` back and checks the pixels: the chosen entry's row is the
  solid bar while the menu is up, and the menu is gone after the pick with the
  cycle showing the chosen entry. The popup button opens a list of its own the
  same way.
