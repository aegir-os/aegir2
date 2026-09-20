# trinket: the GUI toolkit and its first client

Status: decided (2026-09). This is the spec the toolkit's console arc lands
under. `specs/console.md` deferred "the toolkit's look is its own spec"; this
is that spec, and the arc that makes the toolkit real.

`libs/aegir-trinket` has been compiled and linked since the hosted runtime
landed (`specs/cxx.md`), but nothing used it: the greeter drew with its own
18-glyph font and the bureau filled grey. The toolkit spoke the framebuffer's
protocol to the console port and never attached, mapped, listened, painted or
dispatched. This arc gives the toolkit a **real client** — the greeter, rebuilt
on it — so the toolkit's look becomes the system's look and every later
consumer (the bureau, menus, dialogs) has a working base.

## The decisions

- **The toolkit speaks `console.gui`, not the framebuffer.** `Application`
  owns the process's console state: it adopts the spawn kit's memory
  (`aegir-mem`), stands the freeing heap up (`aegir-heap`), attaches one arena
  slice, maps its megapage frames into the process's own window, listens for
  the event channel, and drains the ring. A `Window` owns a backing region
  within that slice and a `Canvas` over it. The framebuffer's `kMethodInfo` is
  never called — the console has no such method, and screen-size is deferred
  (below).
- **The toolkit is the application's entry.** `Application::create` is what a
  service calls first, and it must not allocate before the heap exists: the
  adoption and `aegir::heap::init` happen before the static `Application` is
  constructed. This is the cxx-smoke's ordering (`apps/aegir-cxx-smoke`), and
  it is load-bearing.
- **A window repaints whole.** Tier 1 dispatches `content_->dispatch_paint`
  over the window's `Canvas` on any damage and then calls `console::damage`
  with the window's rectangle. 480×360 is 172800 words and the keystroke that
  caused it costs more than the fill — the greeter's own reasoning, kept.
  Partial damage and a damage-rect union are a later optimization, not a
  correctness need.
- **The look is XEN/Workbench, in the theme.** `theme_xen.cc` already carries
  the palette and metrics; this arc makes them visible. The greeter's
  acceptance pixels change from its hand-picked greys to the theme's roles.
- **The font is embedded and real.** Terminus 12 (`projects/terminus-font`,
  vendored per `specs/third_party.md`) is compiled into the library as a byte
  array by a small build step, and `BitmapFont::load_bdf` is fixed to parse it
  correctly — device advance from `DWIDTH`, and a packed atlas the canvas can
  blit. Fonts are not read from the initrd: the VFS is a service and its file
  syscalls are stubbed (`specs/cxx.md`'s filesystem deferral), so an embedded
  array is the honest source.
- **The first client proves four widgets.** `Label`, `TextBox`, `Button` and
  `Panel`, with focus, key editing, clicks and layout. `MenuBar` waits for the
  bureau's menu server, which the runtime arc removed as speculative.

## The shape

### `Application`

`create()` adopts the bootstrap untyped, VSpace root and address window, then
calls `aegir::heap::init`, then constructs the static `Application` (whose
constructor may now allocate the theme). `exec()`:

1. `attach` a 2 MiB slice, expect one megapage frame, `frame(0)` it, and
   `Scratch::map_large` it into a contiguous region of the process's window;
2. `listen`, taking the event notification into an allocated slot;
3. load the embedded Terminus font as the default;
4. for each visible `Window`, claim a backing region from a bump allocator
   over the slice (which stops short of the ring's last page), `create_window`
   with the real offset, paint, and `damage`;
5. loop: drain the ring and dispatch; when the ring is empty and no posted or
   timer work is due, `seL4_Wait` on the notification;
6. teardown: `destroy_window` each.

`init_display_info` no longer calls the framebuffer protocol. `DisplayInfo`
stays at its defaults (scale 1.0); a window's geometry is the client's to set.

### Event dispatch and focus

The console's packed words (`aegir/console.h`) map to `KeyEvent`/`MouseEvent`:
a translated character becomes `text`, `\b`/`\t`/`\n` become `BACKSPACE`/
`TAB`/`ENTER`; a pointer event's code is its button (with `kButtonRelease`
meaning up) and its value is window-local x and y. Events are routed to the
window whose id they carry, then hit-tested down the content tree with the
container's `child_at`.

Focus is the toolkit's, on top of console's click-to-focus: a `Widget` gains
`focusable()` (true for `TextBox` and `Button`), the window holds the focused
widget, a pointer-down focuses the focusable it lands in, and Tab cycles to
the next. A key event goes to the focused widget; `TextBox::on_submit` and
`Button::on_click` are how a client acts on Enter and a click.

### `Window` and `Canvas`

`Window` owns a backing offset, a `Canvas` over `slice + offset` with the
window's width as stride, and the console window id. `set_content` gives the
content the window's rectangle, lays it out, and paints. `show`/`hide`
create/destroy the console window and return the backing region. Decorations,
`on_moved_resized`, and `update_bureau_window` are the bureau arc's.

### Font

`BitmapFont::load_bdf` parses `DWIDTH` into `Glyph::advance` (the current
parser never sets it, so every advance is zero and text collapses), packs
glyphs into a grid atlas with `atlas_x`/`atlas_y` set (the current parser
leaves them zero and grows one mis-sized buffer), and keeps the BDF metrics.
`Canvas::draw_text` reads the atlas through a virtual on `Font` rather than
casting to `BitmapFont`. `Font::load_terminus` returns the embedded font, and
`Application::load_builtin_font("Terminus", 12)` returns it. `Label` falls back
to the application font when none is set, as `TextBox` and `Button` already do.

## What this is not

Window decorations, title bars, dragging, resizing, depth gadgets, menus and
the menu server; multiple windows per process (the backing allocator
generalizes, but attaching more than one frame waits for a client that needs
it); alpha and blend beyond the canvas's existing per-pixel path; the
screen-size query (the bureau's full-screen window needs it, so it arrives
with the bureau arc); locale and BiDi/RTL (`specs/cxx.md`); a real worker
thread (`specs/cxx.md`'s threading milestone — `WorkerPool` stays lazy); a
clock for `schedule_timer` — the runtime has no user-accessible monotonic
clock, so the event loop consults the clock only when a timer is pending, and
a real source arrives with the threading milestone. Each is easier with the
toolkit standing.

## Acceptance

The greeter, rebuilt on trinket, is the client. Its serial cues are unchanged
— `a name and a secret, please`, `the window has the focus`, `welcome, …` —
and its `auth.login` wire is unchanged, because the credential check never
leaves auth (`specs/auth.md`). The runner's QMP socket reads the form with
`screendump` and asserts the **theme's** pixels rather than the old hand-picked
greys: the window's `WINDOW_BG`, a text field's `INPUT_BG`, the button's
`BUTTON_BG` and a label's `TEXT`. The console backdrop check at (10,10) is the
console's, not the toolkit's, and stays. The runtime's own proof
(`apps/aegir-cxx-smoke`, `CXX_SMOKE_OK`) and the flag-off build are unchanged
and stay the checkpoint.
