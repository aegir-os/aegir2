# trinket/checkbox: the toggle gadgets

Status: decided (2026-09). The first gadgets drawn from MUI's named artwork,
under `specs/trinket/`.

## The problem

`Button` had `Type::CHECK` and `Type::RADIO`, but the indicator was drawn in C++
-- a square well with two hand-written lines for a tick, a circle with a filled
centre -- so the checkbox and the radio looked like the toolkit and not like the
XEN they are meant to match. Phase D named MUI's artwork (`CheckMark`,
`RadioButton`); nothing used it.

## The decisions

- **The indicator is the artwork.** `Theme::draw_check(rect, checked, enabled)`
  and `Theme::draw_radio(...)` draw MUI's `CheckMark`/`RadioButton` image in its
  unchecked or checked frame; the rect is the indicator's own. The theme file
  gives each frame a recipe -- `[gadgets.check.off]` is `{ sprite = "CheckMark" }`
  and `.on` is `{ sprite = "CheckMarkSelected" }`, and the radio's the same with
  its own art -- so the look is the preset's, as every other gadget's is.

- **Two frames, not one.** A MUI mark is a two-frame image: frame 0 the normal
  look and frame 1 the selected one (`MUIA_Selected`). The preset names the image,
  so `scripts/convert_prefs.py` emits both frames, `<Name>` and
  `<Name>Selected`, from the `.mf0`/`.mf1` pair the importer converted.

- **The size is the artwork's.** `CHECK_INDICATOR_WIDTH`/`HEIGHT` and
  `RADIO_INDICATOR_WIDTH`/`HEIGHT` are the art's own sizes at the 96 dpi base
  (23x18 and 17x12), so the widget lays the label out beside the indicator and a
  recipe blits the sprite scaled to the cell -- the same agreement the scrollbar's
  arrow buttons keep with `SCROLLBAR_ARROW_SIZE`.

- **The gadget is the `Button`.** A `Button` of `Type::CHECK` or `Type::RADIO`
  draws the indicator at its left, centred vertically, and the label to its right;
  the preferred width is the indicator plus the gap plus the text. A click or
  Space/Enter toggles `checked_`, as it always did -- the change is where the mark
  comes from, not how it behaves. MUI, too, builds a checkmark as an image button
  with a separate label; `Button` keeps the label inside for convenience.

## The shape

```cpp
// theme.h
virtual void draw_check(Canvas&, const Rect&, bool checked, bool enabled);
virtual void draw_radio(Canvas&, const Rect&, bool checked, bool enabled);

// button.h
enum class Type { PUSH, TOGGLE, CHECK, RADIO };
void set_checked(bool);
std::function<void(bool checked)> on_click;
```

## What this is not

- **A radio *group*.** A radio that un-checks its siblings needs the group to
  own the selection; today a `RADIO` button toggles alone, which is a checkbox
  with a round mark. The group is a later phase.
- **A disabled frame.** The artwork is two frames -- normal and selected -- with
  no disabled variant, so a disabled toggle draws its normal frame; MUI disables
  by not responding, and so does the widget.

## Acceptance

- **`make check-theme`** renders the two checkmark frames and the two radio
  frames on the host beside the reference art, so a wrong frame or a mis-scaled
  indicator is seen in seconds rather than by a boot.
- **The demo** shows a checked checkbox and a radio in a fixed row between the
  terminal and the label band (specs/trinket/layout.md's widget test-bed).
