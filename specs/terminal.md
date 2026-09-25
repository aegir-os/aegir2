# terminal: the CON: handler, its window, and its line

Status: decided (2026-09), and it has landed through Phase 4: the text
surface (`TerminalBuffer`, `TerminalView`, the generated Unicode widths, BiDi
reordering), the terminal process with the line editor and the command line,
the `con.stream` port the terminal serves, the session's spawn kit auth
delegates, and a command resolved to `Initrd:`, spawned out of a reclaimable
pool, printed to the grid, and reported through the runtime that routes its
fd 1/2 and its exit to the stream. fd 0 is the stream's queued input: while a
command runs the terminal routes the keyboard to the stream rather than the
idle editor, and the command's `read` drains it (`aegir-read`). The handler
also rings a client's doorbell -- a notification passed at `open` -- when there
is something to read, so a client can wait instead of poll. The shell is its
own process now (`aegir-shell`): the terminal spawns it once from a pool of its
own, it opens a cooked stream with a doorbell, runs the built-ins, and asks the
terminal to run a command (the terminal holds the spawn authority). This is the
spec
the terminal arc lands under —the Amiga `CON:` handler and the text surface
the shell (a later arc, `specs/shell.md`) runs in. `specs/environment.md` named "the shell and a
`CON:` handler" as future work and left them there; this is that work's first
half.

On the Amiga, `console.device` is the low-level display and keyboard driver,
and **CON: is a handler**: it owns the window, renders the characters, keeps
the line, and hands its clients a character stream. A client — the Shell, a
`Type`, an editor — opens a CON: stream and reads and writes it, and never
touches a window. Several programs can share one CON: window, and the window
outlives any one of them.

Aegir already has the bottom half of that stack. `console.gui`
(`specs/console.md`) is the compositor: it owns the display and the input
devices, carves each client a pixel slice, and delivers packed key, pointer
and focus events. It renders no text and knows nothing of lines. What is
missing is the layer between a program's `printf` and those pixels.

## The separation, decided

**CON: is its own process, and the shell is a client of it.** A per-session
**terminal** process owns one `console.gui` window, a cell grid with
scrollback, the line editor and the history; it serves a character-I/O port.
The shell is a separate process that opens a console stream on that port,
prints a prompt, reads lines, and launches commands. A command is a process
whose standard input and output are the same console stream. The terminal
keeps the spawn authority -- the shell asks it to run a command (`run`), and
reads the command's status back with `command_status` -- so the shell is a
plain CON: client and the command's environment is what the shell sent, not a
second copy the terminal invented.

The alternative — the shell owning the window and rendering its own text —
was rejected, and the reason is not fidelity:

- **A command must print without linking the toolkit.** The moment the shell
  runs an external program, that program's output has to reach the window.
  The only shape that gives *any* program a console is a stream to a process
  that owns the window and renders. Fusing shell and terminal would buy a
  shortcut for the built-in-command case alone and have to be undone before
  the first external command.
- **The window outlives the shell.** A console that a shell owns dies with
  the shell; a console that owns itself can hold a new shell, or several.
- **Pixels stay the console's, text stays the handler's.** The compositor
  never learns to draw glyphs, and the handler never talks to a device. Each
  layer keeps the vocabulary it already has.

The handler is a **session** process, not a boot service: it is per-user, it
holds `console.gui` the way the bureau does, and auth starts it with the
bureau (`specs/auth.md`). A system-wide text service was rejected because
there is no system-wide screen to put it on: windows belong to a session's
badge, and the console is already the one system process that touches the
display.

## The stream, and the two disciplines

A console stream is opened by a client and carries bytes both ways. The
handler renders what the client writes and reads the client's keys.

- **Cooked** (`ReadLine`): the handler owns the line editor. It echoes the
  prompt and the keys as they are typed, keeps the line in a buffer, handles
  the arrow keys (history, cursor), Home/End, Backspace/Delete, and
  **Shift-Backspace clears the line**; Enter ends the line and returns it.
  History is the console's, per stream, grown on demand. This is the mode
  the shell uses, and it is the Amiga console's own command line.
- **Raw** (`Read`): keys are bytes. The handler delivers the character under
  the cursor's key, and a program that wants an editor's control reads raw
  and does its own drawing. The escape vocabulary is small and grows by
  need: `\r`, `\b`, `\t`, `\n`, and erase-to-end-of-line.

Both disciplines share the same output path: a write appends to the grid at
the cursor, wraps and scrolls, and honors `\r`, `\b` and `\n`. A terminal
that only understands the minimal set is what the shell and the first
programs need; a fuller ANSI subset is a later arc, recorded below.

**Why the handler cooks when the Amiga shell also edited.** The Amiga Shell
used the console's own line editor; the console kept the history. Keeping
that in the handler puts the editing in one place, in front of the pixels it
draws to, and leaves the shell a small program that reads lines and runs
them. A program that wants the keyboard without a line asks for raw.

## The shape

### The `con.stream` protocol

Owned by the terminal. Strings travel in the namespace protocol's shape
(`aegir/nmspace.h`); method numbers are the version rule, and a method the
port does not know is answered by saying nothing.

- `open`. In: the mode (cooked or raw) and, in cooked mode, a prompt. Answer:
  one word, `1` opened and `0` refused -- a client has to be able to tell, and
  `listen`'s empty refusal did not leave room for it. The stream is the
  caller's, keyed by its badge, so a badge holds one console; a second `open`
  from the same badge is refused.
- `write`. In: the bytes. They land at the stream's cursor. Reply: the count
  written, less than asked the refusal.
- `read`. In: the most bytes the caller can take (or no word for the
  envelope's bound), so a one-byte key read drains no more than the key. Out:
  bytes, or an empty answer when nothing is queued. The bytes are queued by the
  handler as keys arrive -- while a *command* runs on the stream, every key is
  a byte on its input queue rather than a keystroke for the idle editor
  (`specs/shell.md`'s Phase 4, design A: the command inherits the shell's
  stream and reads it raw). A client with no doorbell polls and asks again; a
  *command* has one (below) and the read parks on it, so a pager waits for a
  key without a poll loop.
- `read_line`. In: nothing. Out: one line, when the line editor has one; an
  empty reply otherwise. This is the cooked call; the shell loops on it. A
  cooked read begins the line editor if it is idle.
- `close`. In: the status the client finished with, which the shell reports
  (specs/shell.md's `return code`). The stream is dropped and the handler
  forgets the line it was holding.
- `exit`. In: the status a *command* finished with. The stream stays open --
  it is the shell's, and a command inherited a copy -- so this is distinct
  from `close`. The handler records it and rings the doorbell; the shell reads
  it with `command_status`.
- `set_prompt`. In: the prompt as a string. A shell that changed directory
  redraws its prompt through this rather than reopening the stream.
- `run`. In: the command line, the current directory and the shell's
  environment, each a string (the environment is the NUL-separated
  `NAME=VALUE` the spawner wants). Answer: one word, `1` started and `0`
  refused. The terminal holds the spawn authority, so the shell asks it to
  start the command -- with the shell's stream, so the output lands on the
  same grid -- and the environment rides in the call because the shell's is
  the shell's.
- `command_status`. Answer: one word, the status, when a command has finished;
  an empty answer otherwise. Reading it clears the finished state, so the
  shell prints one `return code` line and draws the next prompt.
- `size`. Out: two words, the rows then the columns of the text area, so a
  pager sizes a page to the window rather than a constant. It is the one
  attribute of the `get`/`set` family (`title`, later color) that has landed.

The handler wakes a client the way the console wakes its clients: each stream
carries the client's own doorbell -- a notification the client passes as a
capability when it opens the stream (the `listen` shape), which the handler
rings when input is queued, a line is ready, or a command finishes. The
terminal's handler raises `on_wake`; the terminal, which holds the kernel, does
the signal. A toolkit client already waits on the console's event notification
and checks its stream in `on_poll` after each drain (specs/workbench.md's
`Application` shape); a client without that -- the shell as its own process --
waits on its doorbell and reads after each wake.

A *command* has no doorbell of its own -- it inherits the shell's stream -- so
the terminal grants each command a *copy* of one notification as
`con.doorbell` and rings it while a command runs. The runtime's `read` of fd 0
(`aegir-heap`) finds that grant and parks on it, so a command blocks instead of
polling. A line typed while a command ran past what the command read is the
shell's next command: when the shell reads the finished status, the handler
hands the leftover bytes to the line editor, so nothing typed ahead is lost
(`specs/dos.md`'s `more` is the caller this was built for).

**Where the protocol lives.** The wire vocabulary -- the port name, the
method numbers, the modes, the byte bound and the namespace's string shape --
is `aegir/console_stream.h`, and it deliberately pulls no kernel header,
because the handler is a value the host build tests. The client's call
helpers are `aegir/console_stream_client.h`; the handler's side,
`ConsoleStreamServer`, owns the streams and their line editors in the
terminal. One handler serves every client: the wire (the shell and its
commands) and the terminal's own key path, which reaches the same stream
directly because a process cannot call its own endpoint. The terminal also
owns the shell's spawn: `SpawnKit::spawn_shell` starts `aegir-shell` once from
auth's `shell-pool`, and the shell's `run` calls come back to the terminal,
which holds the command pool.

### The terminal's window and render

The terminal is a trinket client (`specs/trinket.md`): `Application`, one
decorated `Window`, the XEN theme, the embedded Terminus font. Its content
is a `TerminalView`, a cell grid whose cell is the font's advance by its
height (Terminus 12 is 6×12, strictly monospace). A cell holds a codepoint
and its attributes; the grid is rows×cols derived from the window's size, less
a **text margin**: the theme draws the frame over the content's edge, so the
first cell sits a few pixels in — a glyph at cell 0 would be cut by the
border.

- **Unicode.** Writes arrive UTF-8 and are decoded a codepoint at a time.
  A codepoint's width is Unicode's: East Asian wide and fullwidth occupy two
  cells, combining marks occupy none and attach to the cell before them,
  format controls occupy none. Widths and combining classes come from the
  pinned UCD, generated at build time, not guessed from the glyph (`the
  font` below). The glyph is looked up separately: a codepoint the font
  cannot draw is still a cell of its width, so a CJK string occupies the
  right amount of the line and renders blank until a fallback font lands.
- **BiDi.** Each visual line is one paragraph: `bidi.cc`'s
  `analyze_paragraph` gives the runs, the runs are reordered for display,
  and mirrored brackets are drawn from `mirror_char`. Mirroring follows rule
  L4 — only a character whose *resolved* direction is RTL (an odd embedding
  level) is mirrored — so an LTR prompt's `>` stays `>`; mirroring every cell
  turned `Home:>` into `Home:<`. The cursor is mapped
  through the same runs, so an arrow key moves visually. This is the first
  consumer of the UAX #9 work (`specs/locale.md`), which until now had none.
- **Scrollback.** Lines scrolled off the top of the grid go to a ring the
  view keeps. The ring grows on demand, to a memory budget the application
  sets — not a line count guessed here (project rule: no arbitrary limits).
  Page Up/Down and the wheel move the view; the view follows the end
  otherwise.
- **Damage.** Only the cells that change are repainted, unioned into one
  rectangle and handed to `console::damage` (`specs/window-manager.md`). The
  grid is a value that knows no view, so the handler is where the two meet: a
  write (`ConsoleStreamServer`) and a key the line editor consumed both damage
  the view, or a client that fills the buffer after `show()` -- the shell's
  banner does -- would never be drawn.
- **A grid is not sized before it is laid out.** `TerminalView::on_layout`
  returns when its rect is empty: a grid sized to a zero rect is one column
  wide, and a client that writes its buffer before `show()` -- the demo's
  grid does -- would wrap every character into its own line.
- **The display order is derived once per change, not once per paint.** A
  repaint of unchanged text is common (a window uncovered, a neighbour's
  damage), and deriving each line's UAX #9 order allocates; recomputing it
  every paint grew the heap until a spawn could not find memory
  (`specs/dos.md`'s leak). `TerminalBuffer` carries a `version`, bumped by
  every content change, and `TerminalView` caches the visible rows' cells,
  recomputing only when the version or the viewport's first line changes.
- **A line that cannot reorder is not analyzed.** The cache is rebuilt on
  every keystroke -- the line editor writes the buffer -- and a full UAX #9
  pass allocates a dozen vectors per visible line, which still grew the heap
  a page a paint. `visual_cells_into` fills the view's own row buffer (its
  capacity reused), and a line whose characters are all outside the classes
  that can reorder or mirror (R, AL, AN, and the explicit controls) keeps the
  logical order without the algorithm. RTL text still takes the full path;
  `scripts/terminal_conformance.cc` asserts both.

### The font, and what it cannot draw

The embedded font is Terminus 12 (`specs/trinket.md`): 1356 glyphs,
Latin/Greek/Cyrillic/Hebrew and box drawing, **no CJK and no Arabic**. The
width and BiDi tables are the Unicode data's, so the *layout* is correct for
those scripts even though the glyph is not drawn. A fallback chain exists
(`Font::add_fallback`) and `font.cc` already names the Noto families for
each script; loading the vendored Noto faces is its own arc, recorded below.
The pager and the shell work with every script the font draws; a script it
does not renders as correctly-sized blanks rather than collapsing the line.

## What this is not

- **The command line and the DOS toolset.** `specs/shell.md` is the shell;
  `SetVar`/`GetVar` and the `ENV:` union stay `specs/environment.md`'s later
  arc.
- **A full ANSI/VT terminal.** Tier 1 understands `\r`, `\b`, `\t`, `\n` and
  erase-to-end-of-line. Colors, alternate screen, cursor addressing, scroll
  regions and mouse reporting are a later arc, added as programs need them.
- **`select`, and a blocking read for a client with no doorbell.** A command's
  read parks on the doorbell the terminal grants it; a client that opens a
  stream without one still polls, and there is no readiness set.
- **Selection, copy and paste.** There is no clipboard service; selecting the
  grid and copying is deferred until one exists.
- **Multiple consoles per session, and windows other than the one.** One
  terminal, one window, one stream per badge to start.
- **Noto fallback fonts** (declared, not loaded).
- **Scrollback persisted across processes.** The handler holds it; closing
  the handler drops it.

## Acceptance

Host-side, the grid is a pure value and is tested like the locale and bidi
work: `scripts/terminal_conformance.cc` compiled and run by
`scripts/check_terminal.py` (`make check-terminal`) asserts cell placement,
wrapping and scrolling, `\r`/`\b` overwrite, wide-character occupancy,
combining zero-width, BiDi reordering and cursor mapping, scrollback
retrieval, and the `con.stream` handler — a line typed into the editor and
read back through the server, a second `open` refused, history and
Shift-Backspace, and the wire path's `open`/`write`/`read_line`/`close` —
whole-run and exact.

On target, the terminal process (`apps/hosted/aegir-terminal`) renders the
grid and the prompt and the runner reads its window back; the runner then
types a line at it through QMP, the shell resolves it to a command and the
terminal spawns it with a caller copy of the console stream, and the runner
reads the command's exit cue (`specs/shell.md`'s acceptance) -- the whole
Phase 3 and 4 path, the output on the same grid the shell writes to. The
runner checks the window holds *ink* -- dark pixels in the text area, not only
solid background -- so a grid that draws nothing, or wraps every character
into one column, fails where the background samples cannot see it. fd 0 is on
the same run: after the hosted command, the runner types a line at the
console while `aegir-read` runs, and the command's exit 0 proves the keys
reached its `read` through the stream's queue. History and the arrows are on
the run too: the runner recalls the last line with the up arrow and edits its
digit with Backspace, and the edited command's exit is the proof the line came
back.
