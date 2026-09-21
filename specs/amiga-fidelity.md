# amiga-fidelity: where Aegir's GUI is not Amiga, and the fixes

Status: living document. Aegir is meant to be Amiga-inspired (`specs/aegir.md`),
and this records the places the GUI diverges from Workbench — what was wrong,
what the Amiga does, and what Aegir does instead. It is a list, not a spec: the
arcs that fix a row record their decisions in their own spec and strike the row
here.

The look of a row is settled against screenshots of Workbench and of the MUI
XEN theme (the theme Aegir uses, `specs/trinket.md`), because some of it — the
gadget fills, the bevels — is hard to describe in words.

## Fixed

### Window chrome (`specs/window-manager.md`)

| | Workbench / XEN | Aegir had | Aegir does |
| --- | --- | --- | --- |
| Gadget order | Close, Zoom, Depth, left to right | Close, Zoom, Depth, all right-packed | Close at the title bar's **far left**; Zoom and Depth at the right, Depth rightmost |
| Title bar | beveled: a light top edge, fill, a dark bottom edge | flat `#0078d7`, no bevel | beveled: highlight `(164,184,215)`, fill **`#6688bb`**, shadow `(67,89,123)` |
| Title text | left-justified, black | left-justified, white | left-justified, black |
| Resizable windows | a bottom bar, same fill, with a resize gadget | a grip in the content's corner | a beveled bottom bar (`(177,194,220)` / `#6688bb` / `(42,56,77)`) with a white right-triangle resize gadget, right angle at the bottom-right and inset from the bevel, and a white separator line |
| Frame | a raised bevel, light top/left, dark bottom/right | one flat border colour | a raised bevel, light top/left, dark bottom/right |
| Close gadget | a small square, white fill | an X | a small square, white fill, near-black outline |
| Zoom gadget | a box in a box: outer `#6688bb`, inner (upper-left) white | a square | a box in a box: outer `#6688bb`, inner (upper-left) white |
| Depth gadget | two cascaded squares: upper `#aaaaaa`, lower white | a down chevron | two cascaded squares: upper `#aaaaaa`, lower white |
| Active / inactive | active fills the gadgets; inactive leaves them hollow | the gadget was the same either way | active fills the gadgets, inactive makes each fill `#6688bb` (hollow), outlines kept |

## Deliberate deviations

- **Menus are always visible, not on the right mouse button.** Workbench shows
  a window's menus in the screen title bar only while the right button is held;
  Aegir draws an always-visible menu bar, because a right-button drag is
  awkward on a modern laptop's touchpad. This is a choice, not an oversight
  (Part 2, `specs/workbench.md`).

## Open

- **The client area's background.** Workbench's window body is `#aaaaaa`
  (`(170,170,170)`); the toolkit's `WINDOW_BG` is `#cccccc`. The greeter's form
  and the bureau read from the theme, so this is a theme value once the panel
  background is settled.
- **The menu model and the screen title bar** (Part 2): the bureau's screen
  title bar, the `bureau.menu` server, and the toolkit's `MenuBar`.
- **Desktop icons and a launcher.**
