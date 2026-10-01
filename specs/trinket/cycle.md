# trinket/cycle: the cycle gadget

Status: decided (2026-09). The second of the value-selection gadgets -- the
slider, the cycle and the popup button -- under `specs/trinket/`.

## The problem

A value with a handful of named choices wants a gadget you *click through*, not
a slider you drag: MUI's `Cycle`. The toolkit had none, and the XEN artwork for
one was imported and unused.

The look is read off a screenshot of the MUI demo's "Cycle Gadgets" group. The
gadget is a **boxed** strip: a black outline around a raised bevel, a small
button cell at the left carrying a mark, a vertical divider, and the current
entry's text centred in what is left. The cell's mark is a small hollow raised
square -- the shape inside `Cycle.mbr`'s left half. `Cycle.mbr` is a 16x11
bitmap that the engine's whole-image blit would distort, so the edge is drawn
from the bevel primitive instead; the `ArrowLeft`/`ArrowRight` pair turns out to
have nothing to do with the cycle and stays unused.

## The decisions

- **A list and an active index.** `Cycle` holds named entries and one of them is
  active. `set_active(index)` changes it without reporting, `on_changed(index)`
  reports a change the *user* made -- the same split the `RadioGroup` keeps
  (`specs/trinket/radio_group.md`): a client setting a value is not a user.

- **It cycles.** A click advances to the next entry and a Shift+click goes back;
  Left and Right do the same when focused, and Home/End jump to the ends. The
  stepping is a pure static -- `step_index(active, count, delta, wrap)` -- so
  `make check-cycle` pins it, including the wrap at either end and a one-entry
  list, which are the cases a click reaches only by arriving.
  `set_wrap(false)` clamps at the ends instead, for a chain that is not a cycle.

- **The cell and the divider are the widget's geometry.** The widget computes
  the divider's line (one pixel, `CYCLE_BUTTON_WIDTH` from the left edge) and
  the mark's small square centred in the cell left of it; the theme draws the
  gadget's face, the divider and the mark into the rectangles it is handed, as
  the scrollbar's theme draws its parts. The active entry's text is the
  widget's, centred between the divider and the right edge, as a `Button` draws
  its label.

- **The popup menu waits.** MUI's cycle also pops its entries up when the text
  is hit; that needs the list a popup opens, which is its own piece. Until then
  a click on the text advances, which is MUI's normal function and not a
  placeholder behaviour.

## The shape

```cpp
// cycle.h
class Cycle : public Widget {
public:
    void add(std::u32string_view entry);
    int count() const;
    const std::u32string& entry(int index) const;

    void set_active(int index);   // programmatic: no on_changed
    int active() const;

    void set_wrap(bool wrap);
    std::function<void(int)> on_changed;   // the user cycled

    static int step_index(int active, int count, int delta, bool wrap);
};

// theme.h -- the face, the divider and the mark are the widget's rectangles and
// the theme's pixels, as the scrollbar's parts are.
virtual void draw_cycle(Canvas&, const Rect& rect, const Rect& divider,
                        const Rect& mark, bool pressed, bool hovered);
```

## What this is not

- **The popup menu.** Above.
- **The label beside it.** MUI's screenshot shows `Computer:` and the gadget as
  two objects; a client puts a `Label` beside a `Cycle`, as the demo does.
- **A disabled frame.** As the checkbox's: the art has none.

## Acceptance

- **`make check-cycle`** (`scripts/check_cycle.py` +
  `scripts/cycle_conformance.cc`): the index stepping, including the wrap, a
  clamp when told not to, and a list with one entry.
- **`make check-theme`** renders a cycle beside the slider, so the box, the cell,
  the divider and the mark are seen on the host.
- **The demo** carries a cycle beside the popup button; the runner clicks it and
  reads `demo: cycle N` back, then checks the pixel its text occupies.
