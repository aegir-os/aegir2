# trinket/layout: the sizing contract and the group layout

Status: decided (2026-09). The first arc under `specs/trinket/` (the landing
spec is `specs/trinket/overview.md`). It gives the toolkit the sizing model its
later widgets are all arranged by, without changing anything on screen.

## The problem

A widget reports one number, `preferred_size()`, and a layout stretches it by an
`Alignment` or a coarse per-index `stretch`. That cannot express MUI's core
idiom -- *the list takes the slack, the buttons keep the size of what they
draw* -- because there is no minimum, no maximum, and no per-child weight. Every
MUI group (`HGroup`, `VGroup`, the "Different Weights" and "Fixed & Variable
Sizes" demos) is exactly this: a weight for the space above each child's
minimum, and a maximum no child may pass.

This arc adds the contract and a group layout that consumes it. It is
deliberately a **no-visual-change** refactor: the existing layouts keep producing
identical rectangles, and the greeter's and bureau's acceptance is untouched.

## The decisions

- **A widget reports three sizes per axis: minimum, preferred, maximum.** All
  three are *content-derived*: they are never read back from the rectangle the
  widget currently holds. (The legacy `Widget::preferred_size` base returns the
  current rectangle; a widget that draws something overrides it, and the two new
  methods default to it, so the base is *fixed* -- minimum = preferred =
  maximum.) A widget with a range to give overrides the maximum; nothing else
  needs to. Two additions serve forms: `set_min_size` is a floor under both the
  minimum and the maximum -- a label column is as wide as the widest label, so
  every field in the form starts at the same x -- and a `TextBox` reports an
  unbounded maximum width, so a host may stretch it to its own edge.

  Content-derivation is what keeps a layout idempotent (`specs/trinket/overview.md`:
  a rectangle change is a damage, a damage is a repaint, a repaint lays out
  again). A layout that ran twice must compute the same rectangles, and it can
  only do that if the sizes it reads do not depend on the rectangles it writes.

- **A container's size is its layout's.** `Container` overrides all three to ask
  `layout_`, falling back to the base when it has no layout. This is what lets
  groups nest: a group is a container, and its parent group reads the same
  contract from it that it reads from a leaf.

- **Weight and cross-axis alignment live in the layout, per child, not on the
  widget.** The same widget in two groups may be fixed in one and free in the
  other; a widget must not need to know which group holds it. This matches the
  existing pattern (`BorderLayout::add_widget`, `AnchorLayout::set_anchor`). The
  record is created on first use and the table grows on demand.

- **`GroupLayout` is one orientation, spaced, with per-child weight/align.**
  Weight is an integer (MUI's own), default **100**. A fixed widget cannot grow
  whatever its weight, because its maximum equals its minimum; a weight of 0
  drops a child that *could* grow out of the distribution. Cross-axis alignment
  defaults to `STRETCH`, so a `VGroup`'s children are as wide as the group.

- **The allocation, exactly.** Let the group's rectangle have main length `L`
  along its orientation and cross length `C`; let `gaps = (n-1) * spacing` for
  the `n` visible children, each with a minimum `min_i`, maximum `max_i`,
  preferred `pref_i`, weight `w_i` and cross alignment `a_i`.
  - The group's own **minimum** is `sum(min_i) + gaps` on the main axis and
    `max(min_i')` on the cross; its **preferred** is `sum(pref_i) + gaps` by
    `max(pref_i')`; its **maximum** is `sum(max_i) + gaps` by `max(max_i')`.
    (The cross uses the per-child cross component.)
  - Placement: each child starts at `min_i`; `free = L - gaps - sum(min_i)`.
    While `free > 0` and some child has `w_i > 0` and `size_i < max_i`, give
    each such child `floor(free * w_i / sum(w))` capped at `max_i - size_i`,
    recompute `free` each pass, and stop when a pass gives nothing. Any pixels
    integer truncation left are handed out one at a time in child order until
    they are used. The children are then placed from the group's origin in
    order, advancing by `size_i + spacing`.
  - On the cross axis a `STRETCH` child fills `C`; a `START`/`CENTER`/`END`
    child keeps its preferred cross size (clamped to `C`) and is placed
    accordingly.

- **The existing layouts are left as they are.** `FlowLayout`, `GridLayout`,
  `BorderLayout` and `AnchorLayout` already size from `preferred_size`, which
  lies within the contract, so their rectangles do not move. `Layout` gains a
  `maximum_size` whose default is the preferred size (they do not grow), and the
  already-present `minimum_size` default stands.

- **No arbitrary limits.** The per-child table is a vector that grows as weights
  and alignments are set; nothing caps the number of children.

## The shape

```cpp
// widget.h
virtual Size preferred_size() const;   // existing
virtual Size minimum_size() const;     // new; default = preferred_size()
virtual Size maximum_size() const;     // new; default = preferred_size()

// Container overrides all three to ask layout_.

// layout.h
class Layout {
    // existing preferred_size is pure; minimum_size default = preferred.
    virtual Size maximum_size(const Container&) const { return preferred_size(container); }
};

class GroupLayout : public Layout {
public:
    enum class Orientation { HORIZONTAL, VERTICAL };
    enum class Align { START, CENTER, END, STRETCH };
    static constexpr int kDefaultWeight = 100;

    explicit GroupLayout(Orientation = Orientation::VERTICAL, int spacing = 0);

    void set_orientation(Orientation);   Orientation orientation() const;
    void set_spacing(int);               int spacing() const;

    void set_weight(Widget* child, int weight);  int weight(Widget* child) const;
    void set_align(Widget* child, Align a);      Align align(Widget* child) const;

    void layout(Container&) override;
    Size preferred_size(const Container&) const override;
    Size minimum_size(const Container&) const override;
    Size maximum_size(const Container&) const override;
};
```

## What this is not

- **The `Group` widget's own geometry.** `Group` has landed -- a `Container`
  with a `GroupLayout`, a frame and a title -- and what it *draws* is the
  theme's (specs/trinket/theming.md). One part is the group's and not the
  theme's: a group box's **title band**. A `GROUP_BOX` with a title reserves
  `font height + 4` at the top of its own rectangle, added to the frame's inset,
  so the layout sizes and places its children below the caption and the group's
  preferred height grows with the font. The caption is **centred and straddles
  the box's top edge**, as MUI's does (read off the "Cycle Gadgets" group's
  screenshot): the group hands the frame a rectangle whose top edge is the
  border line, with half the band above it, and the theme notches that line
  around the caption. So the caption is never over the children, and a larger
  title font cannot push it over the group above.
- **The grey 3-D XEN look.** `theme-xen.md` is the look; this arc changes no
  pixels.
- **Flow and wrap.** `FlowLayout` keeps its own line-breaking; `GroupLayout` is
  a single line by construction.
- **Cross-axis weights or per-child min/max overrides.** A group's cross axis is
  `max` of the children; a child's range is its own.

## Acceptance

- **`make check-layout`** (`scripts/check_layout.py` +
  `scripts/layout_conformance.cc`): a pure host check like `check-atlas`,
  building synthetic widgets over `GroupLayout` and asserting the computed
  minimum/preferred/maximum, the weighted water-fill with and without a maximum
  cap, the cross-axis alignments, a nested group, and idempotence (a second
  layout moves no rectangle).
- **The existing acceptance is unchanged.** The greeter's and bureau's pixels
  are the proof that no existing rectangle moved.
- **The demo is the toolkit's widget test-bed.** Its content is a raised `Group`
  holding a `TabGroup` (`specs/trinket/tabs.md`), one page per kind, so the
  weighted layout runs on the target and the runner reads it back, not only in
  the host check. The widgets land in the demo as they arrive -- a page at a
  time now, not a band; its body sample moved with the frame's two-pixel inset.
