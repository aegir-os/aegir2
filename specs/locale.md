# The locale arc

Status: decided (2026-09). This is `specs/cxx.md`'s completion-program step 6,
"Locale, iconv and BiDi/RTL": the toolkit's internationalization and the C++
standard library's localization. The two are different things and land in this
order, each its own checkpoint.

## What is already there

- **musl's locale and iconv are built.** The vendored upstream musl
  (`projects/musl`, `manifests/sources.toml`) compiles `src/locale/*` and
  `iconv.c`; `setlocale`, `localeconv`, `newlocale`, `nl_langinfo`, `wcsto*` and
  `iconv` all link from `musl_full`. The seL4 fork was configured without them,
  which is why the hosted runtime vendors upstream musl in the first place
  (`specs/cxx.md`). So nothing in the C library needs turning on.
- **libc++ is built with `LIBCXX_ENABLE_LOCALIZATION=ON`**
  (`scripts/build_libcxx.sh`), so `std::locale`'s facets are in the library;
  the time zone database is off, because it wants an IANA zoneinfo tree Aegir
  does not ship. The runtime accepts it (`apps/hosted/aegir-cxx-smoke`).
- **`aegir-trinket`'s `locale.cc` is in the build** but simplified: hardcoded
  English separators and a `n != 1` plural rule, not CLDR data
  (`libs/hosted/aegir-trinket/src/locale.cc`). **`bidi.cc` is now the real
  algorithm** (piece 1) with conformance green; **`translation.cc`** is a gettext
  `.mo` parser that is not yet in the build. `translation.cc` is gated out
  (`libs/hosted/aegir-trinket/CMakeLists.txt`).

## The pieces, in order

1. **UAX #9, the Unicode Bidirectional Algorithm.** `bidi.cc` gains the real
   algorithm, tables generated from the pinned Unicode data, and a conformance
   run against Unicode's own `BidiTest.txt` and `BidiCharacterTest.txt`. This is
   the first landing: it is self-contained (no toolchain risk), it replaces a
   broken stub with something true, and RTL rendering needs it before any
   localized string is drawn.
2. **libc++ localization on.** `LIBCXX_ENABLE_LOCALIZATION=ON`, and whatever
   that pulls in, proven by `std::locale` and `std::stringstream` in
   `apps/hosted/aegir-cxx-smoke`: the classic locale's facets are present, a
   named locale is built through musl's `newlocale`, and a `C.UTF-8` locale's
   `std::codecvt` facet converts UTF-8 through musl's tables. (The proof uses
   that facet rather than `std::wstring_convert` and `std::codecvt_utf8`, which
   C++17 deprecated and libc++ warns on unconditionally -- the same conversion,
   and on the locale rather than a self-contained table.) Depends on 1 only for
   the toolkit.
3. **Trinket's `Locale`, against real CLDR data.** Number, date, currency,
   percent, plural and list formatting from a compiled CLDR subset, replacing
   the hardcoded fields. The `.locale` file format is decided here.
4. **`translation.cc`, gettext for real.** The `.mo` parser reaches the build
   and a toolkit string is translated through it.

The order is a dependency order, not a preference: 3 and 4 want the toolkit's
strings and formatting to have a stable direction, which 1 gives them.

## UAX #9

The algorithm is Unicode Standard Annex #9 in full, not the "simplified" shape
the stub had:

- **Classes.** `BidiClass` gains the four isolate classes (`LRI`, `RLI`, `FSI`,
  `PDI`), so `bidi_class` is the Unicode `Bidi_Class` property and not a
  curiosity. The classes come from `extracted/DerivedBidiClass.txt`; the
  mirroring pairs from `BidiMirroring.txt` plus the canonical U+2329/U+232A
  equivalence; the paired brackets and their types from `BidiBrackets.txt`.
- **The steps.** P2/P3 (paragraph level from the first strong character, or the
  caller's); X1-X8 (explicit levels, overflow and validity), X9 (removing the
  embedding controls and `BN`), X10 (isolating run sequences, BD13) with the
  `sos`/`eos` for each; W1-W7; N0 (bracket pairs, including the canonical
  equivalence and the `EN`/`AN` resolution); N1-N2; I1-I2; L1 (resetting levels
  at separators and paragraph ends); L2 (reordering by reverse levels); and L4
  through `mirror_char`. L3 (combining marks) is a rendering concern and is
  documented with the reordering.
- **The API.** `bidi.h` is reshaped to expose what a caller and a conformance
  test both need, rather than the stub's value-returning `runs()` that computed
  nothing: `analyze_paragraph` returns the resolved paragraph level, each
  character's resolved level, and the L2 visual order; `runs()` becomes a view
  over the stored levels. `BidiDirection`, `BidiClass`, `bidi_class` and
  `mirror_char` keep their names, so `locale.h` and its one consumer are
  unchanged.
- **Acceptance.** A host conformance run: the algorithm is compiled with the
  host compiler (it uses only `<string>`, `<vector>`, `<algorithm>`), and a
  driver reads Unicode's `BidiTest.txt` (class sequences with expected levels
  and reorder) and `BidiCharacterTest.txt` (codepoints, paragraph direction,
  expected paragraph level, levels and reorder) and reports the first
  mismatches. Conformance is whole-file and exact: every test line passes.
  `scripts/check_bidi.py` is the harness, and it fetches the two files by their
  pinned sha256.

## Unicode data, vendored

The Unicode Character Database is third-party and is not committed
(`specs/third_party.md`): it is pinned in `manifests/sources.toml` by sha256 and
fetched by `make deps` (`scripts/fetch_sources.py`), which grows a **file
source** beside its tarball ones -- a list of individually pinned files placed
under `projects/ucd/unicode-16.0.0/`. The three table files
(`extracted/DerivedBidiClass.txt`, `BidiMirroring.txt`, `BidiBrackets.txt`)
come from there; a build-time generator
(`scripts/gen_bidi_tables.py`, an `add_custom_command` like the Terminus font
embed) emits the compact range tables. The two conformance files are pinned in
the same place; `scripts/check_bidi.py` reads them from the fetched tree, so
the version the algorithm is tested against is the version the tables were
built from.

The version is **16.0.0**, the one the UCD files above are pinned at; moving it
is a pin change and a re-run of conformance, not a code change.

## Deferred

- **Shaping.** `bidi.h`'s "Phase 2: Full HarfBuzz shaping" is not this arc; the
  toolkit draws a bitmap font and Arabic shaping waits for an outline font and
  a shaper.
- **The iconv repertoire.** musl's `iconv` builds from its own tables and the
  toolkit needs no conversion beyond UTF-8/UTF-32; a wider repertoire is ICU's
  question, not musl's.
- **Locale data from the system.** `Locale` is built from a compiled subset, not
  read from a system locale database; a locale stored on a volume waits for the
  file syscalls and a reason.
- **CLDR's full data set.** Number/date/plural for the locales Aegir ships, not
  all of CLDR.
