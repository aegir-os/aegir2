# trinket/chrome: the window manager's look, and the rest of the furniture

Status: decided (2026-10). The theming arc's remainder
(`specs/trinket/theming.md`, `specs/amiga-fidelity.md`): after it, no widget
draws a frame, a bevel or a bar of its own -- every part of the furniture is the
theme's, drawn through a `Theme::draw_*` method the engine implements.

## The problem

The theme is data for the *gadgets* -- a button, a field, a scrollbar, a list --
but the **chrome** is still C++ in the widgets. `Window::paint` fills its own
title bar, draws the close/zoom/depth gadgets and the bottom bar's resize
triangle with `fill_rect`/`draw_line` (`window.cc`); `MenuBar::on_paint` fills
the bar and draws each popup row and its accelerator keycap (`menubar.cc`); the
bureau's `Desktop::draw_bar` fills the screen bar (`desktop.cc`). The theme
*declares* `draw_titlebar`, `draw_menubar` and `draw_menu_item` -- and the
engine implements them -- but no widget calls them: they are dead code beside a
second, hand-rolled copy of the same pixels. A theme cannot change the chrome,
and two copies drift.

## The decision: the furniture is the theme's too

Every part of the furniture is a `Theme` method, and the widget hands it a
rectangle and the state:

- **A widget computes geometry; the theme draws the look.** `Window` still
  places the gadgets and sizes the bars -- those are the window manager's
  arithmetic -- but it draws none of it. It calls `draw_titlebar`,
  `draw_gadget(rect, kind, active)` for each of close/zoom/depth,
  `draw_bottombar` and `draw_resize_gadget`, and `draw_window_frame`. The menu
  bar calls `draw_menubar` and, per row, `draw_menu_item`; the bureau's screen
  bar is the same `draw_menubar`. Nothing outside the engine fills a bar or
  draws a gadget.
- **The engine implements each method from the palette, the metrics and a
  recipe.** The look stays data: a new gadget is a recipe in the theme file, not
  a shape in a widget. The engine's own `draw_titlebar`/`draw_menubar`/
  `draw_menu_item`/`draw_window_frame` are the seed; they are completed and the
  rest added in the same shape.

## The vocabulary the chrome needs

The recipe vocabulary is a `fill`, a `bevel`, an `outline`, a `dither`, a
`mark` and a `sprite`, each set into the gadget by a uniform `inset`
(`specs/trinket/theming.md`). The chrome needs two things it cannot say:

- **One edge, not a whole bevel.** The title bar is a fill with a light line one
  pixel down from its top and a dark line along its bottom -- not the four-edge
  `bevel`, and the light line is inset. A `line` step names one edge (`top`,
  `bottom`, `left`, `right`), its colour and its inset: the bar, the bottom bar
  and the menu bar's underline are all this step.
- **A box inside the gadget, not the whole gadget.** A close gadget is an 8x8
  box centred in the 16x16 cell; the zoom gadget is a box in a box; the depth
  gadget is two cascaded squares. A uniform inset draws the first, but not the
  offset inner box or the cascade. A step may carry `at = [x0, y0, x1, y1]`,
  sixteenths of the gadget, naming the sub-rectangle it draws into; an inset and
  an `at` compose (the inset first, then the sixteenths of what is left). The
  close/zoom/depth gadgets are then recipes.

The resize gadget's solid right triangle stays an engine method: it is one
shape, its outline and its separator line are geometric, and a `triangle` step
buys nothing the method does not. `draw_window_frame` keeps its four lines the
same way. The engine methods draw from the palette and the metrics, so the look
-- the colours, the bevels, the bar heights, the gadget sizes -- is still the
theme file's; only a shape with no data form stays in the engine.

## The shape

```cpp
// theme.h -- the chrome joins the gadgets
enum class GadgetKind { CLOSE, ZOOM, DEPTH };
virtual void draw_titlebar(Canvas&, const Rect&, const char* title, bool active);
virtual void draw_gadget(Canvas&, const Rect&, GadgetKind, bool active);
virtual void draw_bottombar(Canvas&, const Rect&);
virtual void draw_resize_gadget(Canvas&, const Rect&);
virtual void draw_menubar(Canvas&, const Rect&);            // already declared
virtual void draw_menu_item(Canvas&, const Rect&, const char* label,
                            bool hovered, bool checked, bool disabled,
                            bool separator);                // already declared
```

```toml
# a line step and a sub-rectangle (see the vocabulary above)
[gadgets.titlebar]
steps = [{ fill = "$TITLEBAR_BG" },
         { line = "top", color = "$TITLEBAR_HIGHLIGHT", inset = 1 },
         { line = "bottom", color = "$TITLEBAR_SHADOW" }]

[gadgets.gadget.close_active]
steps = [{ fill = "$GADGET_WHITE", at = [4, 4, 12, 12] },
         { outline = "$GADGET_OUTLINE", at = [4, 4, 12, 12] }]
```

## Landed

Each was its own landing, the build the checkpoint:

- **A. The window chrome.** The vocabulary extension (`line`, `at`) and its
  generator; the title bar, bottom bar, frame and gadget recipes;
  `Window::paint` through the theme; the preview renders them.
- **B. The menu bar.** `draw_menubar`/`draw_menu_item` completed and called by
  `MenuBar` and the bureau's `Desktop`; the screen bar, the popup well and the
  accelerator keycap are theme methods too.
- **C. The small furniture.** The list's header rule, the text field's caret and
  selection, each through the theme's own method.

## What this is not

- **A widget's own background.** `Widget::on_paint`'s fill, a panel's flat
  background, and the text edit's and the terminal's wells are the widget filling
  its own rectangle with a theme colour, not furniture a theme restyles
  shape-by-shape. They stay.
- **The terminal's cell cursor.** A terminal's inverse cell is its rendering, not
  the window furniture; `Theme::draw_cursor` is the editor's caret, not the
  terminal's block.

## Acceptance

- **The theme preview** (`make theme-preview`) renders the window chrome and the
  menu bar beside the other gadgets, so a chrome change is seen on the host.
- **The target's pixels are the same or re-baselined deliberately.** The window
  chrome's acceptance samples (a title bar `#6688bb`, the frame, a menu row)
  move only if a recipe changes the look; `make run` is the check.
