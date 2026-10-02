# trinket/tabs: the tabbed group

Status: decided (2026-10). The first widget after the file requester, under
`specs/trinket/`. A `TabGroup` shows one page of several with a tab strip along
the top and a framed body. The multiline editor that follows builds on it (a
tab per open file), so the group is general: any titled widget is a page.

## The problem

The toolkit can lay widgets out (`Group`), frame them (`Panel`) and select a
value (`Cycle`, `Slider`), but it cannot show *one of several panels at once*.
A settings window and an editor with a file per tab both want that, and the
requester's arc is over, so it is the next widget.

There is no MUI tab art in the vendored XEN preset, so the look is the theme's
to draw. The reference is a ClassAction tab strip: raised tabs whose **top
corners are chamfered** -- a 45° cut, not a radius -- over a framed page. Under
XEN that becomes the same grey 3-D face a button is, with the cut corner.

## The decisions

- **`TabGroup` is a `Container` of titled pages.** `add_page(title, widget)`
  appends a page (a widget, usually a `Group` or `Panel`); `page(i)` and
  `page_count()` read them; `set_active(i)`/`active()` choose and name the
  shown page, and `on_change(index)` reports a change. **Exactly one page is
  visible** -- the active one; the rest are `set_visible(false)`. That is the
  toolkit's existing rule, not a second one: the group layout, the focus walk
  and the paint all skip an invisible child already
  (`specs/trinket/layout.md`).

- **The strip is along the top.** Tabs lie left to right, in page order, each
  as wide as its title plus `TAB_PADDING_H`; the strip is `TAB_HEIGHT` tall,
  and the body takes the rest. Left, right and bottom strips are deferred, as
  the scrollbar's horizontal was (`specs/trinket/scrollbar.md`).

- **A tab is a chamfered face, the theme's to draw, one colour with the body.**
  The square `draw_bevel` cannot cut a corner, so the tab has its own theme
  method: `Theme::draw_tab(rect, title, active, hovered)` fills the *group's*
  face (`PANEL_BG`), draws the 1px raised bevel with the two **top** corners
  cut by `TAB_CHAMFER`, and centres the title in `TEXT`. The face is the
  body's, not a lighter gadget's: a tab and the frame are one colour, so the
  bevel alone says where a tab is, and **what a page paints inside is the
  app's** -- the group draws no content background of its own. `active`'s
  bottom edge is left open (below); `hovered` is the theme's to ignore, as the
  cycle's is under XEN (`specs/trinket/cycle.md`). The body is `draw_panel`'s
  (`Panel::Style::FRAME`), so a tab group and a framed panel are the same
  pixels.

- **The active tab connects to the body.** The body's top border is the strip's
  baseline. The active tab sits on it with no line between their faces, so the
  two read as one; the inactive tabs keep their full bottom outline. That is
  the reference's shape and what says which page shows.

- **A page is titled, and a client may rename it.** `set_title` is the client's
  when what a page holds changes its name -- an editor saving a file under
  another one. It damages the whole strip, because a title's width is a
  different layout, not one tab's rectangle.

- **A click selects; the keyboard moves.** A click on a tab selects it. The
  strip is focusable: LEFT/RIGHT move the active tab and HOME/END go to the
  ends, and the active page's widgets are focusable while that page shows.
  (Tab order *across* the group -- the strip before the page, a deliberate
  order -- is the toolkit's own milestone, `specs/trinket/overview.md`; for now
  the strip is focusable and the tree order decides.)

- **The strip layout is pure, so a host check pins it.** `tab_layout(...)`
  answers each tab's x and width across a strip width, from the titles'
  pre-measured widths, the padding and the gap -- no font, no theme -- the
  `column_layout` shape again (`specs/trinket/listview.md`). `tab_at(x)` names
  the tab a point is over, so a click and a drawn tab cannot disagree.

## The shape

```cpp
// tab_group.h -- a group of titled pages, one showing
class TabGroup : public Container {
public:
    void add_page(std::u32string_view title, std::unique_ptr<Widget> page);
    int page_count() const;
    Widget* page(int index) const;
    std::u32string const& title(int index) const;
    void set_title(int index, std::u32string_view title);  // a rename

    void set_active(int index);   // one shows; the rest hidden
    int active() const;

    /* The user chose a tab or moved with the keys, not set_active. */
    std::function<void(int)> on_change;

    bool focusable() const override;   // the strip
    ...
};

// theme.h -- the tab's own face; the widget computes the rectangles
virtual void draw_tab(Canvas&, const Rect&, std::u32string_view title,
                      bool active, bool hovered);
```

The metric roles are `TAB_PADDING_H`, `TAB_HEIGHT`, `TAB_GAP` and `TAB_CHAMFER`
(the 45° cut, in pixels); the colours are the gadget's (`GADGET_FACE`,
`GADGET_HIGHLIGHT`, `GADGET_SHADOW`, `TEXT`), no new ones.

## What this is not

- **Sides other than the top.** A strip on the left, right or bottom is the
  same widget with another axis; it waits for a client that wants one.
- **Closable, reorderable or scrolling tabs.** A tab is fixed once added; a
  close gadget, drag-reorder and a strip that scrolls past its width are each
  their own piece. The leading marker the reference's tabs show is the
  *client's* (a dirty mark or a close gadget), not the tab's.
- **A tab per file.** That policy -- an editor opening a file into a tab,
  closing it, a dirty marker -- is the editor's, not the group's.

## Acceptance

- **`make check-tab-group`** (`scripts/check_tab_group.py` +
  `scripts/tab_group_conformance.cc`): the strip layout and `tab_at`'s inverse
  -- widths from the padding and gap, the last tab short of the right edge, a
  point inside each tab naming it -- the pure arithmetic, host-side.
- **The theme** draws the tab in the existing XEN palette; `make theme-preview`
  renders a strip (three tabs over a framed body) beside the other gadgets, so
  the look is seen in seconds on the host (`specs/trinket/theme-xen.md`).
- **The demo is tabbed** (`specs/trinket/layout.md`'s test-bed): its content is
  a `TabGroup`, one page per kind -- Toggles, Values, Lists and Text -- so a new
  widget is a page or a page's child, not another band, and the test-bed grows
  without moving every widget the acceptance reads. The runner clicks a tab,
  reads the cue naming the page that now shows, and reads each page's own
  widgets while it shows: the list and the horizontal bar on Lists, the radio
  pair on Toggles, the terminal and its label on Text, the cycle, the popup
  button and the slider on Values. Each page's gestures drain their cues before
  the next tab, because one page shows at a time -- and a page's cues are read
  before its tab moves on, so a dump never lands on a widget the page below it
  has hidden.
