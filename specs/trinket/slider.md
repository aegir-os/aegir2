# trinket/slider: the slider widget

Status: decided (2026-09). The first of the value-selection gadgets -- the
slider, the cycle and the popup button -- under `specs/trinket/`. The cycle and
the popup button follow.

## The problem

The toolkit had a scrollbar and no slider: a number you choose by dragging a
knob along a track. MUI's XEN draws the two with the same art. The preset
assigns `MUII_PropBack` the dither pattern `0:135` and `MUII_PropKnob` the same
`XEN/Scrollbars/xenbar.image` programme the scrollbar's thumb comes from
(`scripts/prefs.py` names the roles, `scripts/convert_prefs.py` reads the
preset), so the scrollbar's trough and thumb are already the slider's look.

## The decisions

- **A range and a value, not a page.** `Slider` is told `set_range(min, max)`
  and holds a `value` in it; `on_change(value)` reports a change the *user*
  made. MUI's `Prop` is the same shape (`MUIA_Prop_First`/`Entries`/`Current`),
  and the scrollbar's `total`/`page` is the Prop with a visible fraction and the
  arrow buttons added -- so this is the smaller sibling of a widget already
  here, not a new idea.

- **The knob is fixed; the trough is the theme's.** The knob is a fixed
  `SLIDER_KNOB_LENGTH` long -- *not* the page's share, which is what makes a
  scrollbar's thumb grow and shrink -- and rides in the trough's well, set in
  from the container by the same four pixels the scrollbar uses.
  `SLIDER_THICKNESS` is the cross size, the same as `SCROLLBAR_WIDTH`, and the
  theme's `[gadgets.slider.trough]` and `[gadgets.slider.knob]` start as the
  scrollbar's recipes: the look is the preset's, and the two may diverge later
  without touching a widget.

- **The mapping is pure.** `Slider::knob_for(track, min, max, value, knob)` and
  `Slider::value_for_pos(track, min, max, pos, knob)` answer the knob's offset
  and the value a drag to a track offset means, with no canvas and no theme, so
  `make check-slider` pins them -- the same construction as the scrollbar's
  `thumb_for`/`value_for_pos`, for a knob of fixed size (specs/trinket/scrollbar.md).

- **Every affordance is a set.** A drag moves the knob with its grab offset
  kept, so it does not jump under the pointer; a click in the trough moves one
  step (`set_step`) toward the click; and when focused the arrows move one step
  and Home/End go to the ends. All of them funnel through one
  `set_value_from_user`, which clamps and reports, as the scrollbar's
  `scroll_to` does.

- **Both orientations.** `Orientation::HORIZONTAL` is the default -- a slider is
  usually a horizontal thing -- and `Orientation::VERTICAL` is implemented and
  mapped, as the scrollbar's is.

## The shape

```cpp
// slider.h
class Slider : public Widget {
public:
    enum class Orientation { HORIZONTAL, VERTICAL };
    explicit Slider(Orientation = Orientation::HORIZONTAL);

    void set_range(int min, int max);
    int minimum() const;  int maximum() const;
    void set_value(int value);  int value() const;
    void set_step(int step);

    std::function<void(int)> on_change;

    struct Knob { int pos; int size; };
    static Knob knob_for(int track, int min, int max, int value, int knob);
    static int value_for_pos(int track, int min, int max, int pos, int knob);
};

// theme.h -- the trough and the knob are the widget's geometry and the theme's
// pixels, as the scrollbar's are.
virtual void draw_slider(Canvas&, const Rect& trough, const Rect& knob, bool hovered);
```

## What this is not

- **A cycle or a popup button.** The rest of the group; a slider only moves a
  number.
- **A value bubble, ticks or text entry.** MUI's XEN Prop shows a plain knob;
  the value is the client's to label, as the demo does.
- **Pointer capture.** As the scrollbar's: a drag that leaves the widget stops,
  and the grab offset means it resumes cleanly.

## Acceptance

- **`make check-slider`** (`scripts/check_slider.py` +
  `scripts/slider_conformance.cc`): the knob's position and length and the
  drag's inverse, including a degenerate range (`min == max`), a value past
  either end, and a track smaller than the knob.
- **`make check-theme`** renders a horizontal slider beside the scrollbar, so
  the shared XEN trough and knob are seen on the host with the widget's own
  geometry.
- **The demo** carries a slider under the terminal, its value beside it, and
  prints `demo: slider N` when the user moves it. The runner clicks the trough
  to the right of the knob and reads the cue back (`demo: slider 60`), then
  checks the pixels: the knob's raised face at its new place and the trough's
  dither where it sat -- the click, `on_change`, the label and the repaint, end
  to end. The slider sits *inside* the terminal's free row so the bands below
  it keep the geometry the acceptance already reads.
