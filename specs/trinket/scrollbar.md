# trinket/scrollbar: the scrollbar widget

Status: decided (2026-09). The first widget after the look and the group, under
`specs/trinket/`.

## The problem

The toolkit had a scrollbar *look* -- `Theme::draw_scrollbar` and the
`SCROLLBAR_*` roles -- and no scrollbar. The terminal scrolled with Page Up/Down
and nothing showed where in the scrollback it sat.

## The decisions

- **Content, page, value.** A `Scrollbar` is told `set_range(total, page)`: the
  content in `total` units and the viewport's `page` of them. Its `value` is the
  first visible unit, in `[0, total - page]`, and `on_scroll(value)` reports a
  new value when the user moved it. The units are the view's -- lines, rows,
  pixels -- because the scrollbar never counts them, only maps them.

- **The thumb is pure arithmetic.** `thumb_for(track, total, page, value,
  min_handle)` answers the thumb's length (the page's share of the track, never
  below `min_handle`) and its offset (the value's share of the travel), and
  `value_for_pos` is its inverse for a drag. Neither touches a canvas or the
  theme, so `make check-scrollbar` pins them, including the cases a screen shows
  only by luck: nothing to scroll, a page larger than the track's share, and a
  drag past either end.

- **Every affordance is a scroll.** The two arrow buttons move one unit; the
  trough moves a page toward the click; the thumb drags, its grab offset kept so
  it does not jump under the pointer; and when focused, the arrows, Page
  Up/Down and Home/End move it. All of them funnel through one `scroll_to`,
  which clamps and reports.

- **The look is the theme's.** `draw_scrollbar` draws two raised arrow buttons
  at the ends, the trough between them as the MUI XEN **blue/grey dither** (the
  same checker the selection uses) under a black outline, and a **white raised
  thumb** over it -- the bevel primitive and the dither, both
  (`specs/trinket/theme-xen.md`). A new `SCROLLBAR_ARROW_SIZE` metric is the
  button; the widget and the theme agree on it so a click and a drawn arrow line
  up.

- **The demo wires it to the terminal.** In the demo the scrollbar's value is
  the terminal buffer's first visible line and `page` its rows, and a scroll
  sets the buffer's offset to match; the demo re-syncs each poll, so a key or a
  resize is reflected (specs/trinket/layout.md's widget test-bed). The terminal
  and the scrollbar sit in a horizontal group -- the terminal free, the
  scrollbar a fixed strip -- under the label band.

## The shape

```cpp
class Scrollbar : public Widget {
public:
    enum class Orientation { VERTICAL, HORIZONTAL };
    explicit Scrollbar(Orientation = Orientation::VERTICAL);

    void set_range(int total, int page);
    void set_value(int value);
    int value() const;  int maximum_value() const;

    std::function<void(int)> on_scroll;

    struct Thumb { int pos; int size; };
    static Thumb thumb_for(int track, int total, int page, int value, int min_handle);
    static int value_for_pos(int track, int total, int page, int pos, int min_handle);
};
```

## What this is not

- **A scrolling *view*.** The scrollbar is the control; a list or a text view is
  its own widget and owns the offset the scrollbar maps.
- **Pointer capture.** A drag that leaves the scrollbar stops, because the
  toolkit has no capture yet; the grab offset means it resumes cleanly.
- **A horizontal use.** `Orientation::HORIZONTAL` is implemented and mapped, but
  no widget uses it yet.

## Acceptance

- **`make check-scrollbar`** (`scripts/check_scrollbar.py` +
  `scripts/scrollbar_conformance.cc`): the thumb and its inverse, 16 cases.
- **The demo** shows the scrollbar beside the terminal and the terminal's
  scrollback behind it. The runner reads its arrows back from a screendump, then
  clicks the top arrow and reads the demo's `scrolled` cue -- the click, the
  scrollbar, the terminal's scrollback and the repaint, end to end.
