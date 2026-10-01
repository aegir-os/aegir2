# trinket/radio_group: the radio group

Status: decided (2026-09). Mutual exclusion for the toggle gadgets, under
`specs/trinket/`.

## The problem

`Button::Type::RADIO` drew a radio's mark (specs/trinket/checkbox.md), but
nothing kept a set of them exclusive: checking one left the others checked, so a
"radio" was only a checkbox with a round mark. MUI's `Radio` is a framed group
whose children are mutually exclusive, and it is the group that holds the
selection.

## The decisions

- **The group owns the selection.** `RadioGroup::set_active(index)` checks that
  member and clears the others, and `active()` reports it. A member's own
  `on_click` is the group's, so a member has exactly one meaning: it reports that
  it was clicked, and the group decides. A member added on its own -- no group --
  still toggles like a checkbox, which is the `Button`'s own behaviour.

- **A group is never empty.** The first member added becomes the active one, so
  there is no "nothing selected" state to draw or to explain; MUI's Radio starts
  on its first entry too. `set_active(-1)` is refused for the same reason.

- **The arrows move the selection.** The group is focusable and handles Up/Down
  (and Left/Right) by moving the active member, wrapping at the ends. It is the
  group that has the focus, not a member, so the selection moves without a member
  being focused first -- which is what makes a radio group one control rather
  than N.

- **The frame and the title are the group's too.** `RadioGroup` derives from
  `Group`, so `set_frame(Frame::GROUP_BOX)` and `set_title(...)` give MUI's
  `GroupFrameT` look: a framed group whose title sits in the notch, with the
  radios inside. The layout is the group's, as for any group
  (specs/trinket/layout.md).

- **It is not a `Button` mode.** `Button`'s `CHECK`/`RADIO` types are the
  *indicator*; exclusivity is a relationship between widgets, so it lives where
  the relationship does -- the container. A container that could react to its
  children would let the group adopt any `Button`, but the group creating its own
  members is enough and keeps the API honest about ownership.

## The shape

```cpp
class RadioGroup : public Group {
public:
    explicit RadioGroup(Orientation = Orientation::VERTICAL, int spacing = 0);

    Button* add(std::u32string_view text);   // creates the member, joins it
    Button* add(std::string_view text);

    void set_active(int index);              // clears the others
    int active() const;  int count() const;

    std::function<void(int)> on_changed;     // click, arrow, or set_active

    bool focusable() const override { return true; }
};
```

## What this is not

- **Keyboard Tab order among members.** The members are focusable like any
  button, so Tab reaches each; the arrows are the group's, and which of the two a
  keyboard user expects the focus to be on is a decision the toolkit's focus
  model has not made yet.
- **A `Group` hook for children.** `Container::add_child` is not virtual, so a
  group cannot adopt an arbitrary `Button` a caller built and configured; the
  group creates its members. A `on_child_added` hook is a later, generic step.

## Acceptance

- **The demo** shows a radio group in a group box beside the checkbox. The
  runner clicks the second member, reads the demo's cue (`demo: radio 2`), and
  checks the pixels: the second member's ring filled and the first's hollow --
  the exclusivity, end to end. The click took three things to land, and each is
  worth the sentence: a step's `events` are sent only when its `trigger` fires,
  so a click and the cue it produces are two steps, never one; the click must
  happen while the demo is *up*, and the menu dance's own click closes it (the
  titlebar's far left is the close gadget), so the click rides the `restored`
  step, before the screen bar opens the menu; and the point moves with the
  layout -- the group box's new title band pushed the members down.
  `Button::on_mouse_up` now reports a toggle's click on the mouse path as
  `on_key_up` always did on the key path -- before, only a `PUSH` reported.
- **`make check-theme`** renders the checked and unchecked frames of both
  toggles on the host (specs/trinket/checkbox.md).
