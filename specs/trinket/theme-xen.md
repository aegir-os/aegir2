# trinket/theme-xen: the grey 3-D look

Status: decided (2026-09). The toolkit's look, `specs/trinket/layout.md`'s
companion under `specs/trinket/`. It changes what the greeter and the bureau
draw, so the acceptance's theme pixels are re-baselined with it.

## The problem

The theme existed but only half the toolkit drew through it, and the values it
carried were a modern hybrid: `#cccccc` bodies, `#e0e0e0` rounded buttons and a
blue focus ring. The screenshots of the MUI demo under XEN -- the theme Aegir
means to wear (`specs/amiga-fidelity.md`) -- are a different look: the Workbench
3.1 palette, square, hard-edged, with a 1px 3-D bevel and no antialiasing.

## The decisions

- **The palette is Workbench 3.1's.** No new colours; the widgets are built
  from the greys and the chrome blue that are already there:

  | role | value | where |
  | --- | --- | --- |
  | body / group face | `#aaaaaa` | `WINDOW_BG`, `PANEL_BG` |
  | gadget face | `#bfbfbf` | `BUTTON_BG`, `INPUT_BG`, `GADGET_FACE` |
  | highlight | `#ffffff` | `GADGET_HIGHLIGHT` |
  | shadow / outline | `#000000` | `GADGET_SHADOW`, `BORDER` |
  | soft shadow | `#8c8c8c` | `GADGET_SOFT_SHADOW`, `INPUT_BORDER` |
  | text | `#000000` | `TEXT` |
  | chrome blue | `#6688bb` | `ACCENT`, `SELECTION_BG` |

- **A bevel is a primitive.** `Theme::draw_bevel(rect, RAISED|SUNKEN)` draws 1px,
  square, hard-edged: a light top-left and a dark bottom-right for a raised
  gadget, the reverse for a sunken one, over whatever face the caller filled. A
  button, a field, a group frame and (later) a scrollbar's thumb are all this one
  primitive; a theme that wants another look overrides it once.

- **A frame is drawn by the theme, title and all.** `Theme::draw_panel` gains the
  title, so `Panel` and the new `Group` share one implementation and the
  `GROUP_BOX` title is the theme's: the frame is drawn raised, and the title sits
  in a notch cut from its top edge. `Panel::on_paint` now delegates to it (a flat
  panel with its own background keeps that background).
- **A widget carries no frame of its own.** `Button` and `TextBox` call
  `draw_button` and `draw_textbox`; before this they still drew their own
  rounded rects, which left the radius of the old look on a square bevel -- the
  "folded-over" corner on the name field and on the buttons. The widget draws
  its text, its mark and its caret; the frame is the theme's.

- **Focus is the gadget's active state, not a ring.** There is no blue ring: a
  focused or pressed button is drawn inset (a sunken bevel) and a focused field
  gets a full black outline inside its well. `Theme::draw_focus_ring` is empty
  under XEN. This is provisional -- the screenshots show no keyboard focus, so
  the marker is Aegir's choice and easy to change.

- **Selection is dithered.** `Theme::draw_dither(rect, fg, bg)` lays one pixel of
  `fg` on one of `bg`, the Amiga selection, so a list or cycle row is selected by
  a blue/grey checker rather than a blend. No list draws yet; the primitive is
  here for the widget that will.

- **The window chrome stays Workbench.** MUI restyles the window's *interior*,
  not its border; the titlebar, gadgets and frame remain `specs/amiga-fidelity.md`'s
  (`#6688bb` chrome, the Workbench bevels). The window manager's own look is a
  later theming arc.

## The shape

```cpp
// theme.h
enum class Bevel { RAISED, SUNKEN };
virtual void draw_bevel(Canvas&, const Rect&, Bevel);
virtual void draw_dither(Canvas&, const Rect&, Color fg, Color bg);
virtual void draw_panel(Canvas&, const Rect&, Panel::Style, std::u32string_view title, bool);
```

`Group` (see `specs/trinket/layout.md`) takes a `Panel::Style` as its frame and
draws through `draw_panel`, so a group box and a panel are the same pixels.

## What this is not

- **The slider look.** The scrollbar has landed (`specs/trinket/scrollbar.md`):
  a dithered trough, a white raised thumb, and the beveled arrow buttons. The
  slider arrives in the next batch.
- **The window manager's look.** Chrome, per `specs/amiga-fidelity.md`.
- **Hover beyond a lightening.** A hovered button's face is a shade lighter; the
  screenshots show no hover state.

## Acceptance

- **The greeter's and bureau's pixels are re-baselined** in `scripts/targets.py`:
  the body is `#aaaaaa`, a button `#bfbfbf`, the field a sunken `#bfbfbf` well
  with a black outline when focused. `make check-layout` covers the `Group`'s
  frame inset.
