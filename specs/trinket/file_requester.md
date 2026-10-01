# trinket/file_requester: the Amiga file requester

Status: decided (2026-10). The `Requester` shell's first real client, and the
`ListView`'s columns, under `specs/trinket/`. The two pieces it needs from
outside the toolkit are `specs/pattern.md` (the wildcard) and `specs/vfs.md`
(the volumes and `kFlagNoDir`).

## The problem

The Amiga's `Open` requester -- the MUI XEN screenshot -- is a window with a
list of the drawer's entries, a `Pattern` box that filters them by an AmigaDOS
wildcard, a `Drawer` toggle and the current path, a `File` box for the chosen
name, and OK / Volumes / Parent / Cancel. The `Requester` shell already holds
"a caller's content above a bottom button row" (`requester.h`), so what is
missing is the *body* and what it stands on:

- the `ListView` draws one column of text: no titles, no column widths, no
  per-row image, and no date or size beside the name;
- nothing matches an AmigaDOS pattern (`specs/pattern.md`), so the Pattern box
  has nothing to filter with;
- the Volumes button needs the volumes the VFS holds, and which of them are
  browsable -- `NIL:` and `PIPE:` are not, and the browser must not carry a
  hardcoded list of names to hide.

## The decisions

- **It is a `Requester`, and the file list is its body.** The window, the
  bottom button row, the focus and the default/cancel resolution are the
  `Requester`'s (`requester.h`); the file requester supplies the body and the
  four actions. A `MessageBox`, an Execute prompt and this are then the same
  shell with different bodies -- which is what the shell was for.

- **The list grows titles, columns and row images.** `ListView` gains a
  column table -- a title, a width and an alignment each, with the existing
  single text column the empty table -- and each row may name an image the
  theme draws before its first cell. The rows stay one active row and one
  cursor (`specs/trinket/listview.md`); only the row's *contents* widen. The
  image is a theme sprite (the imported `Drawer`, `HardDisk`, `Disk`, `Chip`,
  `Volume` art), so the file list's icons are the look's, as the arrows are.

- **The Pattern box filters with an AmigaDOS wildcard.** The matcher is
  `specs/pattern.md`'s, shared with the shell's globbing and `dir`/`list` --
  not a second one in the requester. The box's contents filter the rows the
  list already holds: the directory is listed once and `pattern::match` keeps
  the names it accepts, so a keystroke is a filter, not a re-list.

- **Volumes enumerates; `kFlagNoDir` is the hide.** The button asks the VFS's
  namespace for `volume_count` and `describe` -- the `Row` is name, flags,
  bound and type -- and lists the volumes whose flags lack `kFlagNoDir`
  (`specs/vfs.md`). The flag is the *volume's* to set: `NIL:` and `PIPE:`
  declare they have no directory, and the browser needs no list of names. A
  volume with a directory (`Initrd:`, a BFS or FAT partition) shows; the
  chooser never names a volume itself.

- **The time comes with the entry, not a `stat` per row.** The requester's
  list shows a name, a size and a date/time, and the date/time is the entry's
  `mtime`. `list` answers `{name, size, kind}` today and `stat` adds the
  `mtime`; the file list wants it for every row it shows, so `list` grows to
  carry it -- one entry becomes `{name, size, kind, mtime}`
  (`kListTailWords` 2 -> 3, `specs/vfs.md`). A filesystem with no clock
  answers zero, as `stat` already allows, and the columns show blank rather
  than a `stat` call per row. This is the arc's one protocol change, and it
  is a change to a writer and its readers at once, not two commits.

- **The body is a widget the requester is handed, not the requester's own.**
  A `FileRequester` builds the body -- the list and its scrollbar, the
  Pattern label and field, the Drawer toggle and path, the File label and
  field -- and the `Requester` hosts it. The VFS is the *client's*: the
  requester takes the `Namespace` (and the drawer it opens on) rather than
  finding a port of its own, so a program that opens a file is the one whose
  manifest holds `vfs.namespace` (`specs/services.md`).

## The shape

```cpp
// listview.h -- the columns the file list needs
struct Column {
    std::u32string title;
    int width;            // pixels; 0 shares what the fixed columns leave
    Alignment align;      // LEFT, CENTER, RIGHT (the existing enum)
};
void set_columns(std::vector<Column> columns);  // empty: one text column
void set_row(int index, std::vector<std::u32string> cells, Icon icon);

// file_requester.h
class FileRequester {
public:
    enum Action { OK = 1, VOLUMES = 2, PARENT = 3, CANCEL = 4 };

    FileRequester(Application& app, aegir::vfs::Namespace& vfs,
                  std::u32string_view title);

    /** The drawer the list opens on; empty opens on the current directory. */
    void open_at(std::u32string_view drawer);

    /** The pattern the box starts with: `#?` lists everything. */
    void set_pattern(std::u32string_view pattern);

    /** The chosen name when OK resolved, empty otherwise. */
    std::u32string const& chosen() const;

    std::function<void(Action)> on_action;
    void show();
    void close();
};
```

## What this is not

- **A save requester, in full.** Open is the arc: the File box is read-only
  until the save case wants a name typed into it, and overwrite confirmation
  is the `Requester`'s message-box shape, not this.
- **The other ASL buttons.** `New Drawer`, `All` and a multi-select list are
  later; the four the screenshot shows are the four here.
- **A pattern walker.** The list is one directory's entries, filtered; a
  pattern does not cross directories and does not recurse
  (`specs/pattern.md`).
- **The volumes as a place to stay.** The Volumes button replaces the list
  with the browsable volumes; picking one opens it as a drawer. It is not a
  second window or a tree.
- **A requester outside the window.** The `Requester` is its own window
  already (`requester.h`); a *popup* file list is the popup layer's
  (`specs/trinket/popup.md`), and nothing here asks for one.

## Acceptance

- **`make check-pattern`** is the matcher's (`specs/pattern.md`); this arc
  adds nothing to it.
- **`make check-listview`** grows the column and image cases: the empty table
  is the one text column, a column table places and sizes its cells, and a
  row's image sits before its first cell.
- **The demo** opens a file requester on a known drawer, reads its columns
  and its list back from a screendump, types a pattern and reads the list
  filtered, presses Volumes and reads the browsable volumes -- with `NIL:`
  and `PIPE:` absent -- and presses OK with a name chosen, reading
  `demo: opened <name>` back. The manifest's `vfs.namespace` and the demo's
  `Open...` menu item are the wiring.
