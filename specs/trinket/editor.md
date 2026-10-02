# trinket/editor: the multiline text editor

Status: decided (2026-10). The widget after the tab group, under
`specs/trinket/`. A `TextEdit` shows and edits a `TextDocument` -- lines of text
with a cursor -- drawing a **block** cursor in insert mode and an **underline**
in overwrite mode, both the theme's. The file-per-tab editor builds on it (a
tab per open file, `specs/trinket/tabs.md`).

## The problem

The toolkit has a form field (`TextBox`) and a terminal grid
(`TerminalView`), but no view for a *document*. `TextBox`'s `multiline` flag
only inserts `'\n'` into one unwrapped string, and the terminal's grid is a
fixed cell matrix that scrolls, not text a person edits. An editor needs lines,
a cursor, insert and overwrite, navigation, and a view that follows the cursor.

## The decisions

- **A `TextDocument` is the model, a `TextEdit` is the view.** The document
  holds the lines (a vector of `u32string`, never empty), the cursor, the mode
  and the edit operations. It is **pure** -- no font, no theme, no rectangle --
  so a host check pins the editing arithmetic the way `tab_layout` and
  `column_layout` are pinned (`specs/trinket/listview.md`, `tabs.md`). The
  widget renders the document, scrolls it, and routes keys and pointer to it.

- **Lines are logical; the view decides how they meet the edge.**
  `set_wrap(false)` is the default: a line wider than the view is **clipped**
  at the right edge and the view scrolls **sideways** to follow the cursor, as
  the terminal clips. `set_wrap(true)` **soft-wraps** a long line at the view's
  width, as Amiga's Ed does. The flag is the component's; clipping is the
  default because it is the simpler model and the honest first cut.

- **The cursor is the theme's, and its shape is the mode's.** Insert mode draws
  a **block** cursor; overwrite draws a **simple underline**. Both are the
  theme's default blue: `ColorRole::CURSOR_BG` (`#6688bb`, the Workbench blue
  the selection and titlebar share) fills the block and draws the underline, and
  `CURSOR_TEXT` (`#ffffff`) is the glyph drawn over a block. The theme draws it
  -- `Theme::draw_cursor(canvas, rect, shape)` -- and the widget computes the
  cell rectangle, the same split as the scrollbar's and the tab's. **Insert is
  the default**; the `INSERT` key toggles, and a client may set it.

- **The view follows the cursor.** The first visible line and, without
  wrapping, the first visible column move to keep the cursor in view. A vertical
  scrollbar is the document's line count with the visible rows as its page, the
  same wiring the terminal's and the list's use
  (`specs/trinket/scrollbar.md`).

- **Selection is its own piece.** Shift+arrows, a `SELECTION_BG` highlight and
  cut/copy are not this cut; the pointer already reports modifiers and
  `click_count`, so the hooks are there. This cut is the cursor, the edits and
  the view.

## The shape

```cpp
// text_document.h -- the model, pure
class TextDocument {
public:
    enum class Mode { INSERT, OVERWRITE };
    explicit TextDocument(std::u32string_view text = U"");

    void set_text(std::u32string_view text);   // '\n' splits the lines
    std::u32string text() const;               // the lines joined by '\n'
    int line_count() const;
    std::u32string const& line(int row) const;

    struct Cursor { int line = 0; int column = 0; };
    Cursor cursor() const;
    void set_cursor(Cursor c);
    Mode mode() const;
    void set_mode(Mode m);
    void toggle_mode();

    void insert(char32_t cp);   // overwrite replaces the cell under the cursor
    void newline();             // split the line at the cursor
    void backspace();           // before the cursor, joining at a line's head
    void erase();               // the cell under the cursor
    void move(int columns, int lines);
    void home();
    void end();

    bool dirty() const;
    std::function<void()> on_change;
};

// text_edit.h -- the view
class TextEdit : public Widget {
public:
    TextDocument& document();
    void set_wrap(bool wrap);       // the default is clipping
    bool wrap() const;
    void set_font(Font* font);
    ...
};

// theme.h
enum class CursorShape { BLOCK, UNDERLINE };
virtual void draw_cursor(Canvas& canvas, const Rect& cell, CursorShape shape);
```

The palette roles are `CURSOR_BG` and `CURSOR_TEXT`. The line height is the
font's, so the rows follow the font as the terminal's do, and the inset is the
terminal view's own four pixels rather than a metric -- the two text views stay
against their frames the same way.

## The application

`aegir-editor` (`apps/hosted/aegir-editor`) is the editor the widget was built
for, and the widget's proof on the target. A launcher starts it as a windowed
program (specs/launch.md's kind 2): its own console.gui and runtime, its own
window. It is the command `edit` in Sys:C, so a shell resolves it like `view`.

- **A file is a tab.** The window's content is the `TabGroup` itself: one page
  per open file, each page the `TextEdit` beside its scrollbar. A file named
  after the program -- `edit Sys:Note.txt` -- opens at start; a new file, or no
  argument, is an `Untitled` page.
- **The File menu.** New, Open..., Save, Save As... and Quit, registered with
  the bureau's menu server as the demo's are (specs/workbench.md), so the screen
  bar carries them while the editor is active. It is the first *launched*
  program to have menus: the spawn kit now carries a caller half of
  `bureau.menu` (specs/launch.md), which the launcher mints each command from,
  where before only a boot service could register one. Open... and Save As...
  raise the toolkit's file requester (specs/trinket/file_requester.md).
- **The namespace is the file layer.** A file's bytes come and go through the
  session's `vfs.namespace` with the runtime's POSIX calls -- `::open`/`::read`
  to load, `::open(O_WRONLY|O_CREAT|O_TRUNC)`/`::write` to save
  (specs/cxx.md step 5).
- **The cues the runner reads.** `editor: ready`, `editor: opened <path>`,
  `editor: saved <path>`, `editor: tab <n>`, and `editor: insert` /
  `editor: overwrite` on the mode toggle -- so the acceptance paces a screendump
  on each and reads the cursor's shape, the text and the tab strip back.

## What this is not

- **A word processor.** No wrapping by word beyond the viewport edge, no
  justification, no styles -- lines are runs of codepoints.
- **Unicode shaping and BiDi.** The canvas draws codepoints LTR today
  (`specs/trinket/overview.md`); the editor inherits that.
- **Undo/redo, search, a status line.** Each is a client of the document, not
  the document.
- **The file, the tab, the menu.** Opening a file into a tab and saving it back
  is the editor *application*'s, not this widget's (`specs/trinket/tabs.md`).

## Acceptance

- **`make check-text-document`** (`scripts/check_text_document.py` +
  `scripts/text_document_conformance.cc`): the pure model -- inserting and
  overwriting, a newline splitting a line, backspace and delete joining lines,
  vertical movement keeping the wanted column, the clamps at both ends.
- **The theme** draws the block and the underline cursor;
  `make theme-preview` renders both beside the other gadgets, so the look is
  seen in seconds on the host (`specs/trinket/theme-xen.md`).
- **The editor application** -- the window with a tab per open file
  (`specs/trinket/tabs.md`) -- is where the widget is proven on the target: the
  runner types into it, presses `INSERT`, and reads the cursor's shape and the
  text back from a screendump: the block's `CURSOR_BG`, the underline's, and the
  glyphs the typing landed. The component itself lands on the two checks above;
  it is not wired into the widget test-bed, whose tab strip has no room left for
  another page.
