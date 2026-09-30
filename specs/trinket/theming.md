# trinket/theming: the theme as data, and the host loop

Status: decided (2026-09). Phase A is landed; B and C follow.

## The problem

The theme was C++ -- `XENTheme::draw_button` and friends -- and matching the XEN
look meant editing pixels in code and waiting ten minutes for a boot to see the
result. That is not a loop anyone can work in, and it produced a scrollbar that
missed the reference several times running: the only oracle was a person
squinting at a screenshot.

## The decision: MUI's own shape

MUI defines a theme as a **Prefs file plus per-gadget artwork**.
`Presets/XEN.prefs` names the art -- `XEN/Plain/11pt/ArrowUp.mf0`,
`CheckMark.mf0`, `XEN/Scrollbars/xenbar.image`, ... -- and the settings;
`Images/xen/**` is the art, IFF ILBM bitmaps. Our theme is the same shape:

- **A theme file** (TOML, like the manifests): the palette, the metrics, the
  fonts, and a **recipe** per gadget -- an ordered list of primitives (`fill`,
  `bevel`, `outline`, `dither`, `mark`, `sprite`, `text`) that the toolkit
  interprets. The look is data, not C++; a theme is swappable and diffable, and
  iterating a bevel is a value, not a rebuild.
- **Sprites** for the bits that are genuinely art: MUI's ILBM bitmaps, imported.
- **Recipes** for the procedural bits (the scrollbar's frame, panels), drawn by
  the primitives the toolkit already has (`draw_bevel`, `draw_dither`, ...), so
  the interpreter is small and the C++ `draw_*` overrides go away.

**96 dpi is the base.** Theme numbers are at 96 dpi; the Amiga is 75 dpi, which
is only how a screenshot of it is read.

## The loop (Phase A, landed)

Iterating in C++ behind a boot is the failure; the loop is a host one, in the
family of `check-atlas`/`check-layout`/`check-scrollbar`:

- `scripts/ilbm.py` -- an ILBM decoder (BMHD/CMAP/BODY, ByteRun1, masking).
- `scripts/import_theme_art.py` -- converts the MUI XEN artwork to the theme's
  sprites under `libs/hosted/aegir-trinket/resources/themes/xen/`, mirroring the
  source tree. The converted PNGs are committed (the MUI package itself never
  is); `.image` files are Amiga programmes -- the scrollbar's drawing is one --
  and are skipped.
- `scripts/theme_conformance.cc` + `scripts/check_theme.py` -- compile the
  toolkit's real `theme.cc`/`theme_xen.cc`/`canvas.cc` with the host compiler,
  render every gadget onto one `Canvas`, and write `out/theme/preview.png`
  (twice size) beside `out/theme/reference.png` (the imported art, same scale).
- `make theme-preview` (and `make check-theme`).

A gadget change is seen in seconds, and the reference is a picture to compare
against.

## What this is not (yet)

- **Phase B**: the theme file and its interpreter; the `XENTheme` C++ becomes the
  interpreter of `themes/xen.toml`, with the values taken from `XEN.prefs`.
- **Phase C**: sprite drawing (blit / nine-slice) so the imported art is what the
  toolkit draws for the buttons and marks, and a converter from `XEN.prefs`.
- The scrollbar is re-done through the mechanism once B and C land; until then
  the host loop is what makes that possible at all.

## Acceptance

`make check-theme` compiles and runs the host render and requires a
non-degenerate sheet (more than the background's few colours); `preview.png` and
`reference.png` are the side-by-side the eye checks.
