# trinket/listview: the list widget

Status: decided (2026-09). The first of the list/viewer arc, under
`specs/trinket/`. It is the widget the cycle's menu and the popup button wait
for, and the first caller of the selection dither.

## The problem

The toolkit has no list: a column of rows, one of them chosen. The XEN artwork
for one was imported and unused, and the theme's `Theme::draw_dither` -- "the
Amiga selection ... for a list or cycle row" -- had no caller.

The look is read off the MUI screenshot of a cycle's expanded popup, which is a
listview: a black outline around a grey face, rows of black text, and the
**selected row a solid `SELECTION_BG` bar with the text still black** (it is a
palette screen; nothing is inverted). That is a correction to what
`specs/trinket/theme-xen.md` assumed: the preset gives `ListSelect` a *colour*
(`0:144`), and the *patterns* it gives `ListCursor`/`ListSelCur` (`2:m1`,
`2:m7`) are the **cursor**, the row being pointed at, not the selection.

## The decisions

- **Rows, an active row and a cursor.** `ListView` holds a list of row strings.
  `active` is the row the user chose and `set_active` is a client's own choice
  that does not report; `cursor` is the row the pointer points at. Clicking a
  row makes it both; moving over another row moves the cursor alone, so a
  selected row and a pointed-at row can differ, which is what the two looks are
  for. `on_select(row)` reports a choice the *user* made, the split the
  `RadioGroup` and the `Cycle` keep.

- **The selection is solid, the cursor is dithered.** `[gadgets.list.selected]`
  fills the row with `SELECTION_BG`; `[gadgets.list.cursor]` lays the
  blue/grey dither over the face. The row's text is drawn by the widget in
  `TEXT` (black) either way -- the palette screen does not invert it -- as a
  `Button` draws its label.

- **The text's alignment is the host's, and the list owns its own height.** A
  row's text is left-aligned by default -- a file list is -- and `set_align`
  moves it; a menu's entries are centred, which is how MUI's read
  (specs/trinket/popup.md). `height_for_rows(n)` is the height that shows `n`
  rows whole, the well included: a host that counts rows and forgets the well
  asks for one row less than it needs, which is how the cycle's menu first came
  up two rows tall with the entry the pick aimed at below the fold.

- **Columns, titles and a row image.** A row is one text cell by default; a
  *column table* (`set_columns`) gives it a titles row inside the well and
  per-column cells (`set_row`), each cell placed and aligned in its column. A
  column's width is pixels, or 0 for an equal share of what the fixed columns
  leave -- the free column is the one that grows, which is how a file list's
  name column takes the width its size and date columns do not. The titles row
  is the list's own band: `height_for_rows` counts it and `row_at`/
  `visible_rows` leave it out. The arithmetic is pure (`column_layout`), so the
  host check pins it -- the gap is a parameter, the theme's `SPACING_SMALL` on
  the target. Columns are separated by that gap: a right-aligned cell ends at
  its column's right edge and the next column's left-aligned cell begins at that
  same edge, so with no gap the two touch (a size against the date beside it).

  A row may carry an `Icon` (`aegir/trinket/icon.h`): the theme draws the
  imported MUI drawer/volume art in a leading strip the columns start after.
  The art is a small *wide* bitmap, so `draw_icon` fits it to the strip rather
  than stretching it to a square. The requester's file list is the client
  (`specs/trinket/file_requester.md`).

- **The look is the theme's, the rectangles the widget's.** `Theme::draw_list`
  draws the well (a black outline around the gadget face), and
  `Theme::draw_list_row(rect, state)` draws one row in its state
  (`NORMAL`/`CURSOR`/`SELECTED`). The widget computes the visible rows from its
  rectangle and the theme's row height and hands the theme one row rectangle at
  a time, as the scrollbar hands it its parts -- so a click and a drawn row line
  up.

- **The scrolling is arithmetic, and the bar is the existing one.** `first` is
  the first visible row; `visible_rows()` is the rectangle's share of the row
  height; `set_first` clamps to `[0, count - visible_rows]`. The `Scrollbar` is
  the control, as it is for the terminal's scrollback: the client wires
  `set_range(count, visible)` and `set_first`, and the list does not own one.
  The rectangle is known only once the list is laid out, so the list reports
  each change of what it shows with `on_visible_changed` and the client
  re-syncs: a range set before layout carries `visible` zero and pins the
  thumb to its minimum, which reads as far more rows than there are. The pure
  mapping -- `first` and the clamped value -- is host-checked.

- **Every affordance is a move.** A click selects the row under it; the arrows
  move the selection a row, Page Up/Down a page, Home/End to the ends, and the
  move scrolls the list to keep the selection visible. A client may still set
  `first` directly, because a scrollbar drag is not a selection.

## The shape

```cpp
// listview.h
class ListView : public Widget {
public:
    void add(std::u32string_view text);        // the simple shape: one text cell
    const std::u32string& row(int index) const;

    // The columned shape (specs/trinket/file_requester.md): a titles row and
    // per-column cells, with an optional row image.
    struct Column { std::u32string title; int width; Alignment align; };
    void set_columns(std::vector<Column> columns);
    void set_row(int index, std::vector<std::u32string> cells, Icon icon = Icon::NONE);
    std::u32string const& cell(int index, int column) const;
    Icon row_icon(int index) const;

    // The columns' x and width across a row: pure, so the host check pins it.
    struct ColumnLayout { std::vector<int> x; std::vector<int> width; };
    static ColumnLayout column_layout(std::vector<Column> const&, int width, int padding,
                                      int gap);

    void set_active(int index);   // programmatic: no on_select; -1 for none
    int active() const;
    void set_cursor(int index);
    int cursor() const;

    void set_first(int index);
    int first() const;
    int visible_rows() const;     // the rectangle's share of the row height

    std::function<void(int)> on_select;   // the user picked a row

    int row_height() const;       // the theme's: the font's line and its pad
    int row_at(Point p) const;

    /* The first visible row a scroll to `value` wants, clamped to the range
     * (specs/trinket/listview.md); pure, so the host check pins it. */
    static int clamp_first(int value, int count, int visible);
};

// theme.h
enum class ListRow { NORMAL, CURSOR, SELECTED };
virtual void draw_list(Canvas&, const Rect& rect);
virtual void draw_list_row(Canvas&, const Rect& rect, ListRow state);
```

## What this is not

- **A format string.** MUI configures its listview's columns with one
  (`MUIA_List_Format`); Aegir names them explicitly, which is what a file list
  needs. A string that builds a column table waits for a client that wants to
  configure one.
- **The image as a column.** MUI's row image is the first format column's;
  Aegir's is a row's own, drawn in a leading strip, which is what a file list's
  drawer/volume icons want.
- **The popup.** A list drawn *above* a window's content -- what the cycle's
  menu and the popup button want -- needs an overlay the window composites
  last, which is its own piece (`specs/trinket/cycle.md`,
  `specs/trinket/popup_button.md`).
- **Multiple selection and drag-and-drop.** One active row; `MUIA_List_MultiSelect`
  and MUI's drag-and-drop wait for a client that wants them.

## Acceptance

- **`make check-listview`** (`scripts/check_listview.py` +
  `scripts/listview_conformance.cc`): the scroll mapping -- `first` clamped to
  the range, an empty list, a list shorter than the view, and a count that shrank
  under the scroll offset -- and the columns' layout: fixed widths in order, a
  free column taking what they leave, two free columns sharing it, a fixed
  column past the width leaving nothing, and the padding at either end.
- **`make check-theme`** renders the well, a normal row, a cursor row and a
  selected row, and the six row icons beside the imported drawer/volume art, so
  the solid selection, the dithered cursor and the icons are seen on the host.
- **The demo** carries a list above the cycle's row, with its own scrollbar. The
  runner clicks a row and reads `demo: list N` back, checking the pixels: the
  chosen row is the solid bar and the row that gave it up is the face again.
  Then it moves over another row, scrolls with the arrow and reads
  `demo: listed N` back, checking that the chosen row moved up with the rows.
  The **dithered cursor** is `check-theme`'s to prove rather than the target's: a
  cue follows the *choice* and the *scroll*, never the pointing, so a dump cannot
  catch the cursor between them.
