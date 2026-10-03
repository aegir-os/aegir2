# fonts: `Sys:Fonts` and the font service

Status: decided (2026-09). Phase 1 (fonts on disk) lands with this spec;
FreeType and the service are phases 2 and 3. `specs/trinket/overview.md` owns the
toolkit's `Font` interface; this is where the fonts it draws come from.

The toolkit drew exactly one font before this arc: Terminus 12, compiled into
`libaegir-trinket` as a byte array, with `Application::load_builtin_font(
"Terminus", 12)` its only source and the theme's four roles (`font()`,
`font_small()`, `font_large()`, `font_monospace()`) all returning it. Nothing
read a font from the system, and no TrueType or OpenType face could be drawn.
This arc gives the system fonts: a tree, a service that owns them, and a
toolkit that asks for one.

The arc has landed, and the theme's `[fonts]` table names the roles' families
and sizes -- Noto Sans 11 for the UI, Noto Sans Mono 11 for a grid -- each
loaded once through the service (`Application::font_for`, a BDF directly and an
OpenType face through `font.main`) and cached. Terminus is the embedded
fallback a boot with no face keeps, no longer the default
(specs/trinket/theming.md).

## The decisions

- **Fonts live in `Sys:Fonts`, and the tree is scanned recursively.** The path
  is for a person navigating it; a face is found by its **own metadata**, not
  by where it sits -- the BDF's `FAMILY_NAME`, `WEIGHT_NAME`, `SLANT` and
  `PIXEL_SIZE`, or a TrueType/OpenType `name` table reached through the file's
  table directory. That name is read directly, and FreeType is kept for the
  rendering: opening a face to ask its name reads megabytes through a VFS
  window of 936 bytes, which was twenty seconds of a boot (below). A subfolder
  named after a family is a convenience and nothing more; moving the file
  between folders changes nothing.
- **The toolkit's `Font` is the seam.** `BitmapFont` (a BDF/PCF parsed in the
  app) and `ServerFont` (a client of the font service) both implement it, and
  the widgets never learn which they hold. This is what makes the mechanism
  below a decision that can be revisited without touching a single client.
- **A font service owns FreeType, the index and the cache, and is a
  rasterizer.** `font.main` loads faces from `Sys:Fonts`, keeps the family index
  and a glyph cache, and answers metrics and glyph bitmaps. FreeType's own file
  layer is never used: its stream is ours, a descriptor that reads through the
  VFS on demand. That is not a detail. A face can be nineteen megabytes (the CJK
  collection) and one VFS read carries 936 bytes
  (`aegir::volume::kReadMax`), so a memory face is tens of thousands of round
  trips and the whole collection resident, where a stream reads the tables a
  face needs -- kilobytes to index it, and only what a render touches. It is the
  closest thing to what fontconfig does on Linux, where FreeType also reads only
  the tables it needs and the expensive scan is paid for once; there the once is
  an on-disk cache, here it is the service's own index, which no client rescans
  (a persistent cache is the escape hatch if `Sys:Fonts` ever gets large). The
  service does **not** shape text: shaping is HarfBuzz's and grows later behind
  `Font::shape`, which is the seam for it. One FreeType, one face set, one glyph
  cache; an app holds only a thin client and its atlas.
- **Glyphs cross on a client-owned transfer page, batched.** The client maps a
  page, hands the service a capability to it, and asks for a list of
  codepoints; the service fills the page with the glyphs and answers their
  metrics. The client copies them into its own atlas. A page the client owns
  has no window in which the service could reuse it under a preempted client's
  copy; a server-owned page would need a lease or a generation to say the same.
  The page is also why a batch fits: the IPC envelope is 119 words, a 48-point
  glyph is not.
- **A missing face is usable, and loud.** The embedded Terminus falls back in,
  and both the service and the toolkit log the family that was asked for. A
  blank window is the worst way to report a missing font: it is
  indistinguishable from every other failure. Keeping the one font we already
  ship as the fallback also covers the path before `Sys:` is up.
- **FreeType is vendored as a signed release tarball.** A `[[source]]` in
  `manifests/sources.toml` -- the release archive, its sha256, its detached
  signature, the signing key committed (`manifests/freetype-signing-key.asc`)
  and its fingerprint pinned, exactly as musl is (`specs/third_party.md`).
  `scripts/build_freetype.sh` builds it out of tree with CMake into a static
  archive for the target, against the hosted runtime's full musl, with its
  optional dependencies off (`FT_DISABLE_ZLIB`, `_BZIP2`, `_PNG`, `_HARFBUZZ`,
  `_BROTLI`): Aegir ships none of them, and a module that wanted one should
  fail to link rather than fall back at run time. The autotools path's
  single-object build (`FT_MAKE_OPTION_SINGLE_OBJECT` in 2.14) has no CMake
  equivalent, so the archive is many objects; the service links one archive and
  the modules its faces reach, which is the trade for not carrying a second
  build system for one library.
- **The subset the system volume ships.** NotoSans (Latin, Greek and
  Cyrillic), NotoSansMono, NotoSansArabic, NotoSansHebrew, and NotoSansCJK's
  `.ttc` -- one file holding ten faces, and the only real CJK face the vendored
  tree has. Roughly 21 MB, which the AEGIR volume carries; the CJK faces are
  large as a class, and a built subset (pyftsubset) is the escape hatch if the
  size ever bites. The subset is a *choice*, not a limit: any face dropped into
  `Sys:Fonts` is found by the same scan.
- **The disk builder refuses a non-font file.** The vendored `noto-fonts` tree
  holds an HTML page where a CJK `.ttc` should be (a bad fetch). Packing it
  would embed 9 KB of HTML as a font and fail at the first load, far from the
  cause; the builder checks a file's magic against its extension and stops.
- **The volume's name index is the ceiling, and it is BFS's.** A BFS volume's
  name index is one B+tree node, and the faces under `Sys:Fonts` are enough
  names to overflow the format's 1024-byte node. What our builder writes
  instead -- a 2048-byte node, which our own reader takes from the header --
  and the multi-level index that lifts the ceiling for good are recorded in
  `specs/bfs.md`; a built subset (pyftsubset) is the other lever, and either
  way it is a *volume* limit, not a font one.

## The shape

### `Sys:Fonts` (phase 1)

```
Sys:Fonts/
    Terminus/ter-u12n.bdf      (and the sizes and weights worth shipping)
    Noto/NotoSans-Regular.ttf
    Noto/NotoSansMono-Regular.ttf
    Noto/NotoSansArabic-Regular.ttf
    Noto/NotoSansHebrew-Regular.ttf
    Noto/NotoSansCJK-Regular.ttc
```

Phase 1 packs and reads the BDF faces; the OpenType files land on the volume
with them and are skipped by the catalog until phase 2 can read them (an
extension it does not know is not an error).

### The toolkit's catalog (phase 1)

`Application::exec` scans `Sys:Fonts` once, before the windows are created,
and builds an index of `FontFace`s: family, weight, slant, pixel size and
path. A `*.bdf` is probed from its header -- `FAMILY_NAME`, `WEIGHT_NAME`,
`SLANT`, `PIXEL_SIZE` -- reading kilobytes and not the face, because the CJK
collection beside it is 19.5 MB. `load_font(family, size, bold, italic)`
answers the exact face, then the nearest size, then nothing; a caller that
gets nothing falls back to the embedded Terminus and logs the miss. The theme's
roles ask for the families and sizes a theme names.

The pure half -- parsing a BDF header, and choosing a face from a list -- is
host-tested (`scripts/check_fonts.py`), the way the allocator and the terminal
grid are.

### The `font.main` port (phase 2)

Owned by `aegir-font`, started by director before any session draws (the
greeter is auth's and comes later). Methods, in the namespace protocol's
string shape:

- `open`. In: family, size, bold, italic. Out: 1 and a face id, or 0 when no
  face matches (the caller falls back).
- `metrics`. In: the id. Out: 1 and the ascent, descent, line gap and height,
  the descent negative as FreeType's metrics are.
- `close`. In: the id.

**Landed.** The service scans `Sys:Fonts` recursively and reads each face's own
family and style -- a BDF's properties, or a TrueType/OpenType `name` table
reached through the table directory, a collection's being a directory per face
(`apps/hosted/aegir-font/src/probe.{h,cc}`, host-tested over the real faces by
`scripts/check_font_probe.py`). FreeType answers `open` and follows: it reads
through a stream that is ours, a descriptor over the VFS, and it is opened only
when a client asks for a face. A request's face is chosen by family, then the
size nearest the ask, then the style. It waits for `Sys:Fonts` before it
indexes, and its first proof is its own: it opens a face by name, reads its
metrics and checks them for the shape metrics must have (a positive ascent, a
non-positive descent, a line height at least the ascent -- `height` is line
spacing, not ascent minus descent, so they are checked apart).

**What the direct read bought.** Indexing through FreeType cost twenty seconds
of a boot (`16 faces from Sys:Fonts in 19974 ms`) because opening a CFF face
reads megabytes through the VFS window, and it left the acceptance depending on
the console's timing at `BOOT_TIMEOUT=480`. Reading the name directly is
**425 ms** for the same sixteen faces. A face is still opened -- through
FreeType -- when a client asks for it, which is where the megabytes belong.

A disk cache in the fontconfig shape (a persisted index, invalidated when
`Sys:Fonts` changes) is the next step if the tree ever grows enough to want
one; the probe is what makes the boot cheap until then, and the index is the
same one a cache would persist.

- `glyphs`. In: the id, a list of codepoints, and a capability to the client's
  transfer page. Out: 1, how many codepoints were answered, and a record each --
  advance, bearing, bitmap box, the byte offset within the page, and whether the
  face has the codepoint. **Landed in the service.** The page bounds the pixels
  and the kernel's envelope bounds the records, so an answer is a *prefix* of a
  request: a caller whose batch was trimmed asks again for the rest.

The transfer page is a frame the client owns, maps, and hands over for one
call, and the service maps a *copy* of before writing. The copy is not a
formality: a frame capability pins to the VSpace it is first mapped into --
`seL4_RISCV_Page_Map` refuses one already mapped elsewhere with
`seL4_InvalidCapability`, "already mapped in a different VSpace". So the
capability that crosses is a copy made before the client's own mapping, and
`seL4_CNode_Copy` is what makes it: a derived frame capability carries no
mapping (`Arch_deriveCap` clears the ASID and address). Two capabilities to one
frame is the kernel's documented way to share a page
(`kernel/manual/parts/vspace.tex`). The service unmaps and drops its copy when
the call is answered, so the page is the client's alone between calls -- which
is what makes a client-owned page cheaper than a server-owned one, which would
need a lease or a generation to answer the same question.

**Landed, both sides.** The service's half is above. The client's is
`ServerFont` (`libs/hosted/aegir-trinket/src/server_font.cc`), the toolkit's
`Font` on the serving side: it opens a family through the service, takes a
transfer page of its own -- retyped with `Allocator::alloc_page`, so the
toolkit names no architecture -- maps it, and makes the unmapped copy each call
hands over. A glyph is fetched on first use and copied from the page into the
client's own atlas, which the canvas blits from, so a widget draws a `.ttf`
face with no rasterizer of its own. `measure` fetches a run's codepoints in
batches, so a label that measures its text and then draws it pays one call per
batch rather than one per glyph.

A process reaches the service through the manifest: `font.main` in a client's
`needs` is what puts the caller half in its bootstrap block, and director's
`rights_for` gives that caller **Grant** -- without it the transfer page
silently does not cross and the service refuses the call (`ipc/port.h`). The
demo's label is the first client: it draws Noto Sans 16 beside its grid, and
its cue is the box of the glyph the service sent back.

### Shaping (phase 3)

HarfBuzz, BiDi and the Noto faces by script: a shaped run (positioned glyphs,
clusters) becomes a `font.main` call or an in-app library over the service's
faces. `Font::shape` is the seam, and which side of it HarfBuzz lands on is
this phase's decision -- shaping needs the face's OpenType tables, which a
service-backed client does not otherwise hold.

## The phases

1. **Phase 1 -- fonts on disk.** `make_disk.py` packs `Sys:Fonts` from the
   vendored trees with the magic check; the toolkit scans it, loads the
   default from it, and falls back to the embedded Terminus with a log when a
   family is missing. BDF/PCF only. No service, no FreeType.
2. **Phase 2 -- FreeType and the service. Landed.** The signed-tarball pin, the
   build script and the service: `aegir-font` reads the volume's faces directly
   and serves `open`, `metrics`, `close` and `glyphs`, all sixteen indexed in
   429 ms, and it rasterizes a glyph into the caller's own page. The client is
   `ServerFont`, so the toolkit draws a `.ttf`/`.otf`/`.ttc` face through it,
   and the demo's label is the first caller.
3. **Phase 3 -- shaping and the fallback chain.** HarfBuzz, BiDi, and the Noto
   faces chosen per script.

## What this is not

A dynamic library loader. One shared library (FreeType) does not pay for a
loader, a shared text segment and relocations; the service gives one copy
without them, and libraries want their own arc with more than one client. Also
not: a font *manager* (installing, enabling, disabling faces -- the scan finds
what is there), a text stack that lays out paragraphs, or DPI-scaled rendering
(a bitmap face is fixed; a scalable one is drawn at the size asked, and DPI
scaling is the scale arc's).

## Acceptance

Phase 1: the toolkit logs the face it loaded and the runner cues on it -- a BDF
read directly, or the served face (`trinket: font <family> <size> from
font.main`); a host conformance asserts the header probe and the selection
(exact family/size and style, the nearest size, the miss). The theme's default
is Noto Sans 11 now, so the acceptance's ink checks were re-baselined to the
face's metrics; the clicks did not move, because they follow the rect cues the
widgets report (specs/testing.md). A site that unpacks a face into `Sys:Fonts`
gets it drawn without a rebuild -- that is the property the phase exists for.

Phase 2: the service's own cues are its scan count, its metrics check on Noto
Sans 16 and the box of a rasterized glyph; the client's is the demo's
`outline A <box> advance <n>`, which is a glyph crossing a client-owned transfer
page and landing in the client's atlas -- the seam, end to end. Host
conformances assert the name probe over the real faces (`check-font-probe`) and
the glyph layout (`check-atlas`): where a served glyph lands is what the canvas
reads back, a copy that scans the wrong stride or keeps the wrong row draws a
smudge on a screen no cue is asserting -- which is exactly what the first one
did -- and a glyph with no box (an outline face's space) that does not move the
pen runs the words together. The look is unchanged by design: the outline face
lands in a widget beside the grid, so the phase lands the mechanism without
moving the samples the acceptance reads.
