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
  single text column the empty table -- and each row may name an `Icon` the
  theme draws in a leading strip. The rows stay one active row and one cursor
  (`specs/trinket/listview.md`); only the row's *contents* widen. A width of 0
  is the free column, so the name column takes the width the size and date
  columns leave. The image is the imported `Drawer`/`HardDisk`/`Disk`/`Chip`/
  `Volume` art, fitted to the strip rather than stretched (it is a small wide
  bitmap), so the file list's icons are the look's, as the arrows are.

- **The Pattern box filters with an AmigaDOS wildcard.** The matcher is
  `specs/pattern.md`'s, shared with the shell's globbing and `dir`/`list` --
  not a second one in the requester. The box's contents filter the rows the
  list already holds: the directory is listed once and `pattern::match` keeps
  the names it accepts, so a keystroke is a filter, not a re-list. It is the
  *file* list's filter: Volumes lists every browsable volume whatever the box
  holds, because a volume is not a name the wildcard picks from the open
  drawer.

- **Volumes shows the block volumes, then the assigns, under a header.** The
  columns are `Label | Device | % Full | Available Space | Used Space`, so the
  list keeps its titles row -- a table whose header the Amiga would not draw,
  but whose numbers are unreadable without it. The list opens with the
  browsable volumes: the label, the backing block device's name (the namespace
  `Row`'s `device`, the block protocol's identify name), and the capacity the
  volume's `space` answers. The two sizes and the percent are the locale's:
  `Locale::format_size` (the largest decimal digital unit, to one decimal, in
  the locale's own unit words and number form -- "8.9 MB", fr's "8,9 ko") and
  `Locale::format_percent` (specs/locale.md), so the requester owns no size
  formatter. A volume that will not resolve or refuses `space` keeps its
  capacity cells blank. Then the namespace's bindings as the Amiga's Assign rows: the assign's
  label (`C:`, `Home:`, ...) under `Label`, the literal word `Assign` under the
  second column -- not the path it stands for, which a binding can hold several
  of -- and the capacity columns blank. The columns swap for the visit and swap
  back when the file list returns.

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

- **The control rows line up.** The Pattern box, the Drawer toggle and the File
  box share one label column: its width is the widest leading widget, each label
  is right-aligned in it, the field takes the rest, and a spacer the scrollbar's
  width ends every field at the list's edge rather than the body's. A form's
  labels line up because the column is a width, not because the labels happen to
  measure the same.

- **The listing is folders first, then the filesystem's collation.** Directories
  sort before files, and each group sorts alphabetically in the manner of the
  filesystem behind it -- BFS case-sensitively, FAT with its ASCII fold
  (`specs/vfs.md`). The volume does not report which collation it uses yet, so
  the sort is case-sensitive today; a per-volume signal is the follow-up.

- **Volumes toggles.** A first press lists the browsable volumes; a second press
  leaves them and lists the drawer again. OK and Cancel still close.

- **The body is a widget the requester is handed, not the requester's own.**
  A `FileRequester` builds the body -- the list and its scrollbar, the
  Pattern label and field, the Drawer toggle and path, the File label and
  field -- and the `Requester` hosts it. The VFS is the *client's*: the
  requester takes the `Namespace` (and the drawer it opens on) rather than
  finding a port of its own, so a program that opens a file is the one whose
  manifest holds `vfs.namespace` (`specs/services.md`).

- **Volumes and Parent act on the open dialog.** The `Requester` closes on a
  button, which is what OK and Cancel want; the file requester asks it to keep
  the window up for Volumes and Parent (`Requester::set_stays_open`), which list
  the volumes and walk to the parent drawer in place.

- **The `Drawer` toggle is the directories.** Checked -- its default -- the
  drawer's directories and files are listed; unchecked it hides the
  directories, leaving the files the pattern keeps. The path box beside it is
  the drawer: Enter in it opens the path, and a click on a directory row opens
  it, as the Amiga's requester does.

- **The path and the size are pure, so a host check pins them.** The parent of
  a path, the join of a drawer and a name, and a size as `List` writes it (a
  comma every three digits) are `file_path.h`'s, with no theme and no VFS, so
  `make check-file-path` covers them without a boot.

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

// file_path.h -- the pure helpers, host-checked
std::u32string parent_of(std::u32string_view path);
std::u32string join(std::u32string_view drawer, std::u32string_view name);
std::string format_size(uint64_t bytes);

// file_requester.h
class FileRequester {
public:
    enum Action { OK = 1, VOLUMES = 2, PARENT = 3, CANCEL = 4 };

    FileRequester(Application& app, aegir::vfs::Namespace& vfs,
                  std::u32string_view title);

    /** The drawer the list opens on. It is listed at once, so a drawer that
     *  does not resolve leaves an empty list. */
    void open_at(std::u32string_view drawer);

    /** The wildcard the list is filtered by, `#?` (everything) at first. */
    void set_pattern(std::u32string_view pattern);

    std::u32string const& drawer() const;
    /** The name in the File box when OK resolved, empty otherwise. */
    std::u32string const& chosen() const;

    std::function<void(Action)> on_action;
    void show();
    void close();
    bool visible() const;
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

- **`make check-pattern`** is the matcher's (`specs/pattern.md`) and
  **`make check-listview`** the columns' (`specs/trinket/listview.md`); this arc
  adds nothing to either.
- **`make check-file-path`** (`scripts/check_file_path.py` +
  `scripts/file_path_conformance.cc`): the parent of a path, the join of a
  drawer and a name, and a size as `List` writes it.
- **The demo** opens a file requester on a known drawer, reads its columns
  and its list back from a screendump, types a pattern and reads the list
  filtered, presses Volumes and reads the browsable volumes -- with `NIL:`
  and `PIPE:` absent -- and presses OK with a name chosen, reading
  `demo: opened <name>` back. The manifest's `vfs.namespace` and the demo's
  `Open...` menu item are the wiring.
