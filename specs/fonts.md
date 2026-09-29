# fonts: `Sys:Fonts` and the font service

Status: decided (2026-09). Phase 1 (fonts on disk) lands with this spec;
FreeType and the service are phases 2 and 3. `specs/trinket.md` owns the
toolkit's `Font` interface; this is where the fonts it draws come from.

The toolkit has exactly one font: Terminus 12, compiled into
`libaegir-trinket` as a byte array, with `Application::load_builtin_font(
"Terminus", 12)` its only source and the theme's four roles (`font()`,
`font_small()`, `font_large()`, `font_monospace()`) all returning it. Nothing
reads a font from the system, and no TrueType or OpenType face can be drawn.
This arc gives the system fonts: a tree, a service that owns them, and a
toolkit that asks for one.

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
  transfer page. Out: each glyph's advance, bearing and rectangle within the
  page. **Next.**

The transfer page is a frame the client maps, hands over for the call, and the
service maps a *copy* of before writing: a frame cap pins to the VSpace it is
first mapped into, so a page two processes read and write is two capabilities
to one frame, which is the kernel's documented way to share a page
(`kernel/manual/parts/vspace.tex`). The copy is unmapped when the call is
answered, so the page is the client's alone between calls -- which is what
makes a client-owned page cheaper than a server-owned one, which would need a
lease or a generation to answer the same question.

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
2. **Phase 2 -- FreeType and the service.** The signed-tarball pin, the build
   script and the service have landed: `aegir-font` reads the volume's faces
   directly and serves `open`, `metrics` and `close`, all sixteen indexed in
   425 ms. What remains is `glyphs` and the client-owned transfer page, and the
   client `ServerFont` that draws a `.ttf`/`.otf`/`.ttc` face through them --
   the client side of the seam phase 1 left open.
3. **Phase 3 -- shaping and the fallback chain.** HarfBuzz, BiDi, and the Noto
   faces chosen per script.

## What this is not

A dynamic library loader. One shared library (FreeType) does not pay for a
loader, a shared text segment and relocations; the service gives one copy
without them, and libraries want their own arc with more than one client. Also
not: a font *manager* (installing, enabling, disabling faces -- the scan finds
what is there), a text stack that lays out paragraphs, or DPI-scaled rendering
(the bitmap faces are fixed; scalable faces arrive with phase 2).

## Acceptance

Phase 1: the toolkit logs the face it loaded and the runner cues on it; a host
conformance asserts the header probe and the selection (exact family/size and
style, the nearest size, the miss); the existing look is unchanged, because
the Terminus the disk carries is the byte-for-byte face the embedded array
held. A site that unpacks a face into `Sys:Fonts` gets it drawn without a
rebuild -- that is the property the phase exists for.
