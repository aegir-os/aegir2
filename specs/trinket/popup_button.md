# trinket/popup_button: the popup button

Status: decided (2026-09). The last of the value-selection gadgets -- the
slider, the cycle and the popup button -- under `specs/trinket/`.

## The problem

A small button that opens something -- a file chooser, a drawer view, a list --
is MUI's `Popobject` family, and the XEN preset gives it a whole-button image
each: `PopUp` (a magnifier), `PopFile` (a document), `PopDrawer` (a drawer),
22x17, in a normal and a selected frame. The artwork was imported and unused.

## The decisions

- **The image is the whole button.** Unlike the checkmark, which is an
  *indicator* beside a label (`specs/trinket/checkbox.md`), a MUI popup button's
  image already carries its 3-D face and its outline, so the widget draws the
  sprite as its whole self and nothing else. `POPUP_BUTTON_WIDTH`/`HEIGHT` are
  the art's own size at the 96 dpi base, so a recipe blits the sprite into the
  cell -- the same agreement the toggles' indicators keep with their art.

- **Two frames, and the role picks the pair.** A MUI mark is two frames, normal
  and selected (`MUIA_Selected`); `scripts/convert_prefs.py` already emits both
  (`<Name>`/`<Name>Selected`). The widget's `Role` -- `POPUP`, `FILE`, `DRAWER`
  -- picks the pair, and the selected frame is drawn while the button is
  pressed, which is the state the art has for it.

- **The widget reports a click; the popup waits.** `on_click` fires on release,
  as a `Button`'s does, and what it opens -- a list, a requester -- is the
  popup's own, larger piece (the same list the cycle's menu waits for,
  `specs/trinket/cycle.md`). The button is honest about being one: it is the
  look and the press, and the object it pops is the client's.

## The shape

```cpp
// popup_button.h
class PopupButton : public Widget {
public:
    enum class Role { POPUP, FILE, DRAWER };
    explicit PopupButton(Role = Role::POPUP);

    std::function<void()> on_click;
    bool focusable() const override { return true; }
    Size preferred_size() const override;   // the art's own size
};

// theme.h
virtual void draw_popup(Canvas&, const Rect& rect, PopupButton::Role role,
                        bool selected);
```

## What this is not

- **The popup.** Above; the list or requester it opens is its own piece.
- **A text label beside the image.** A MUI popup is the image alone; a client
  that wants a labelled one puts a `Label` beside it.

## Acceptance

- **`make check-theme`** renders the three roles in both frames on the host, so
  a wrong frame or a mis-scaled button is seen in seconds.
- **The demo** carries a popup button beside the cycle. The runner clicks it,
  reads `demo: popup` back and checks the magnifier's own colour where the art
  puts it; then, with the object open, checks the ink of its three centred
  entries and picks the second (`Save`), reading `demo: picked 2`. The click is a
  step of its own, after every cue the demo's burst produced: a popup-opening
  gesture inside that burst is dismissed -- and swallowed -- by the click that
  follows it.
