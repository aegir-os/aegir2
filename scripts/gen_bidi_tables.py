#!/usr/bin/env python3
"""Generate the toolkit's UAX #9 tables from the pinned Unicode data.

    python3 scripts/gen_bidi_tables.py <ucd-dir> <out.cc>

Reads `extracted/DerivedBidiClass.txt`, `BidiMirroring.txt` and `BidiBrackets.txt`
from the UCD tree `make deps` fetched (`manifests/sources.toml`) and writes the
compact C++ tables `bidi.cc` looks characters up in. The output is generated,
never committed: the build runs this as a custom command, and the conformance
harness runs it too, so the algorithm and the data are always the same version
(specs/locale.md).

Stdlib only, so the host needs nothing but python3.
"""

from __future__ import annotations

import sys
from pathlib import Path

# Field order is the Bidi_Class property's abbreviated names, mapped to the
# BidiClass enumerators in aegir/trinket/bidi.h. DerivedBidiClass.txt uses the
# abbreviations; its @missing lines use the long names, mapped here.
ABBREVIATIONS = {
    "L": "L", "R": "R", "AL": "AL", "EN": "EN", "ES": "ES", "ET": "ET",
    "AN": "AN", "CS": "CS", "NSM": "NSM", "BN": "BN", "B": "B", "S": "S",
    "WS": "WS", "ON": "ON", "LRE": "LRE", "LRO": "LRO", "RLE": "RLE",
    "RLO": "RLO", "PDF": "PDF", "LRI": "LRI", "RLI": "RLI", "FSI": "FSI",
    "PDI": "PDI",
}

LONG_NAMES = {
    "Left_To_Right": "L", "Right_To_Left": "R", "Arabic_Letter": "AL",
    "European_Number": "EN", "European_Separator": "ES",
    "European_Terminator": "ET", "Arabic_Number": "AN",
    "Common_Separator": "CS", "Nonspacing_Mark": "NSM",
    "Boundary_Neutral": "BN", "Paragraph_Separator": "B",
    "Segment_Separator": "S", "White_Space": "WS", "Other_Neutral": "ON",
    "Left_To_Right_Embedding": "LRE", "Left_To_Right_Override": "LRO",
    "Right_To_Left_Embedding": "RLE", "Right_To_Left_Override": "RLO",
    "Pop_Directional_Format": "PDF", "Left_To_Right_Isolate": "LRI",
    "Right_To_Left_Isolate": "RLI", "First_Strong_Isolate": "FSI",
    "Pop_Directional_Isolate": "PDI",
}

MAX_CODEPOINT = 0x10FFFF


def parse_semicolon_fields(line: str) -> list[str]:
    """The fields of a UCD data line, comments and whitespace stripped."""
    line = line.split("#", 1)[0].strip()
    if not line:
        return []
    return [field.strip() for field in line.split(";")]


def class_table(path: Path) -> bytearray:
    """One Bidi_Class byte per code point, from the ranges and @missing lines.

    The file is derived data: explicit ranges miss unassigned code points, which
    take the default from the @missing lines. A flat array is built and then
    run-length encoded, which avoids an interval-merge bug and costs nothing
    here.
    """
    order = list(ABBREVIATIONS)
    index = {name: i for i, name in enumerate(order)}
    codes = bytearray([index["L"]]) * (MAX_CODEPOINT + 1)

    for raw in path.read_text(encoding="utf-8").splitlines():
        stripped = raw.strip()
        if stripped.startswith("#"):
            marker = stripped.find("@missing:")
            if marker < 0:
                continue
            body = stripped[marker + len("@missing:"):]
            fields = parse_semicolon_fields(body)
            if len(fields) != 2:
                continue
            span, name = fields
            if name.startswith("#"):
                name = name[1:].strip()
            long_name = name.split("#", 1)[0].strip()
            if long_name not in LONG_NAMES:
                continue
            first, _, last = span.partition("..")
            start = int(first, 16)
            end = int(last, 16) if last else start
            for cp in range(start, end + 1):
                codes[cp] = index[LONG_NAMES[long_name]]
            continue
        fields = parse_semicolon_fields(raw)
        if len(fields) != 2:
            continue
        span, name = fields
        if name not in index:
            raise SystemExit(f"unknown Bidi_Class {name!r} in {path}")
        first, _, last = span.partition("..")
        start = int(first, 16)
        end = int(last, 16) if last else start
        for cp in range(start, end + 1):
            codes[cp] = index[name]

    return codes


def run_lengths(codes: bytearray) -> list[tuple[int, int, int]]:
    """Collapse the per-code-point array into (first, last, class) ranges."""
    ranges: list[tuple[int, int, int]] = []
    start = 0
    for cp in range(1, len(codes) + 1):
        if cp == len(codes) or codes[cp] != codes[start]:
            ranges.append((start, cp - 1, codes[start]))
            start = cp
    return ranges


def pairs(path: Path, fields: int) -> list[tuple[int, int]]:
    """The (code point, second field) pairs of a two-field data file."""
    result: list[tuple[int, int]] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        values = parse_semicolon_fields(raw)
        if len(values) < fields:
            continue
        try:
            result.append((int(values[0], 16), int(values[1], 16)))
        except ValueError:
            continue
    result.sort()
    return result


def brackets(path: Path) -> list[tuple[int, int, bool]]:
    """The (code point, paired code point, is-open) triples of BidiBrackets.txt."""
    result: list[tuple[int, int, bool]] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        values = parse_semicolon_fields(raw)
        if len(values) < 3:
            continue
        try:
            cp = int(values[0], 16)
            paired = int(values[1], 16)
        except ValueError:
            continue
        result.append((cp, paired, values[2] == "o"))
    result.sort()
    return result


def emit(ucd: Path, out: Path) -> None:
    codes = class_table(ucd / "extracted" / "DerivedBidiClass.txt")
    ranges = run_lengths(codes)
    order = list(ABBREVIATIONS)
    mirrors = pairs(ucd / "BidiMirroring.txt", 2)
    bracket_pairs = brackets(ucd / "BidiBrackets.txt")

    lines: list[str] = []
    lines.append("/* Generated by scripts/gen_bidi_tables.py -- do not edit.")
    lines.append(" * Source: Unicode Character Database 16.0.0, pinned in")
    lines.append(" * manifests/sources.toml (specs/locale.md).")
    lines.append(" */")
    lines.append("")
    lines.append('#include "bidi_tables.h"')
    lines.append("")
    lines.append("#include <cstddef>")
    lines.append("")
    lines.append("namespace aegir::trinket::detail {")
    lines.append("namespace {")
    lines.append("")
    lines.append("struct ClassRange { char32_t first; char32_t last; BidiClass cls; };")
    lines.append("")
    lines.append("constexpr ClassRange kClassRanges[] = {")
    for first, last, cls in ranges:
        lines.append(
            f"    {{0x{first:X}, 0x{last:X}, BidiClass::{order[cls]}}},"
        )
    lines.append("};")
    lines.append("")
    lines.append("struct MirrorPair { char32_t from; char32_t to; };")
    lines.append("")
    lines.append("constexpr MirrorPair kMirrorPairs[] = {")
    for cp, mirror in mirrors:
        lines.append(f"    {{0x{cp:X}, 0x{mirror:X}}},")
    lines.append("};")
    lines.append("")
    lines.append("struct BracketPair { char32_t cp; char32_t paired; bool open; };")
    lines.append("")
    lines.append("constexpr BracketPair kBracketPairs[] = {")
    for cp, paired, is_open in bracket_pairs:
        lines.append(f"    {{0x{cp:X}, 0x{paired:X}, {str(is_open).lower()}}},")
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace")
    lines.append("")
    lines.append("BidiClass bidi_class_lookup(char32_t c)")
    lines.append("{")
    lines.append("    std::size_t lo = 0;")
    lines.append("    std::size_t hi = sizeof(kClassRanges) / sizeof(kClassRanges[0]);")
    lines.append("    while (lo < hi) {")
    lines.append("        std::size_t const mid = (lo + hi) / 2;")
    lines.append("        if (c < kClassRanges[mid].first) {")
    lines.append("            hi = mid;")
    lines.append("        } else if (c > kClassRanges[mid].last) {")
    lines.append("            lo = mid + 1;")
    lines.append("        } else {")
    lines.append("            return kClassRanges[mid].cls;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return BidiClass::L;")
    lines.append("}")
    lines.append("")
    lines.append("bool mirror_lookup(char32_t c, char32_t *out)")
    lines.append("{")
    lines.append("    std::size_t lo = 0;")
    lines.append("    std::size_t hi = sizeof(kMirrorPairs) / sizeof(kMirrorPairs[0]);")
    lines.append("    while (lo < hi) {")
    lines.append("        std::size_t const mid = (lo + hi) / 2;")
    lines.append("        if (c < kMirrorPairs[mid].from) {")
    lines.append("            hi = mid;")
    lines.append("        } else if (c > kMirrorPairs[mid].from) {")
    lines.append("            lo = mid + 1;")
    lines.append("        } else {")
    lines.append("            *out = kMirrorPairs[mid].to;")
    lines.append("            return true;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return false;")
    lines.append("}")
    lines.append("")
    lines.append("bool bracket_lookup(char32_t c, char32_t *paired, bool *open)")
    lines.append("{")
    lines.append("    std::size_t lo = 0;")
    lines.append("    std::size_t hi = sizeof(kBracketPairs) / sizeof(kBracketPairs[0]);")
    lines.append("    while (lo < hi) {")
    lines.append("        std::size_t const mid = (lo + hi) / 2;")
    lines.append("        if (c < kBracketPairs[mid].cp) {")
    lines.append("            hi = mid;")
    lines.append("        } else if (c > kBracketPairs[mid].cp) {")
    lines.append("            lo = mid + 1;")
    lines.append("        } else {")
    lines.append("            *paired = kBracketPairs[mid].paired;")
    lines.append("            *open = kBracketPairs[mid].open;")
    lines.append("            return true;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return false;")
    lines.append("}")
    lines.append("")
    lines.append("}  // namespace aegir::trinket::detail")
    lines.append("")

    out.write_text("\n".join(lines), encoding="utf-8")
    print(
        f"INFO  bidi tables: {len(ranges)} class ranges, {len(mirrors)} mirrors, "
        f"{len(bracket_pairs)} brackets",
        flush=True,
    )


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip())
        return 2
    emit(Path(argv[0]), Path(argv[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
