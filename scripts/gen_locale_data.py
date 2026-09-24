#!/usr/bin/env python3
"""Compile the pinned CLDR data into the toolkit's .locale blobs.

    python3 scripts/gen_locale_data.py <cldr-dir> <locales.toml> <out.cc> [<locale-files-dir>]

Reads the locales `manifests/locales.toml` names, loads their CLDR JSON from the
tree `make deps` fetched (`manifests/sources.toml`), and writes two things: the
C++ source `<out.cc>` the toolkit embeds (one byte array per locale and a
lookup), and, when a directory is given, one `<locale>.locale` file per locale.
The `.locale` file bytes are exactly the array the C++ carries, so
`Locale::load` reads the same format the toolkit embeds (specs/locale.md).

The format is a flat, extensible property table: a small header, then a
sequence of key/value UTF-8 strings. A reader that does not know a key ignores
it, so a new field -- locale data from a later CLDR release, or a field this
generator learns to emit -- needs no format version bump. List-valued
properties join their elements with U+001F (unit separator).

Stdlib only, so the host needs nothing but python3.
"""

from __future__ import annotations

import json
import sys
import tomllib
from pathlib import Path

MAGIC = b"AEGLOC1\n"
FLAG_RTL = 0x0001
SEPARATOR = "\x1f"

# A field the language file does not carry. CLDR's root locale would supply
# these; cldr-json does not publish root (manifests/sources.toml), so the
# defaults are documented here. The nine shipped languages are complete for the
# fields compiled, so these are a floor, not the common path.
DIGIT_DEFAULT = "0123456789"


def load(path: Path):
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def numbering_system(numbers: dict) -> str:
    return numbers.get("defaultNumberingSystem", "latn")


def number_properties(cluster: Path, loc: str) -> dict[str, str]:
    numbers = load(cluster / "cldr-numbers-full" / "main" / loc / "numbers.json")[
        "main"
    ][loc]["numbers"]
    system = numbering_system(numbers)
    symbols = numbers.get(f"symbols-numberSystem-{system}", {})
    properties = {
        "numbers.decimal": numbers[f"decimalFormats-numberSystem-{system}"]["standard"],
        "numbers.percent": numbers[f"percentFormats-numberSystem-{system}"]["standard"],
        "numbers.currency": numbers[f"currencyFormats-numberSystem-{system}"]["standard"],
        "numbers.scientific": numbers[f"scientificFormats-numberSystem-{system}"]["standard"],
        "numbers.symbol.decimal": symbols.get("decimal", "."),
        "numbers.symbol.group": symbols.get("group", ","),
        "numbers.symbol.percent": symbols.get("percentSign", "%"),
        "numbers.symbol.minus": symbols.get("minusSign", "-"),
        "numbers.symbol.plus": symbols.get("plusSign", "+"),
        "numbers.symbol.exponential": symbols.get("exponential", "E"),
        "numbers.symbol.permille": symbols.get("perMille", "\u2030"),
        "numbers.symbol.infinity": symbols.get("infinity", "\u221e"),
        "numbers.symbol.nan": symbols.get("nan", "NaN"),
    }

    # The digits the default numbering system writes numbers in. latn is ASCII;
    # an algorithmic system (none is a default here) has no digit string and
    # would fall back.
    systems = load(cluster / "cldr-core" / "supplemental" / "numberingSystems.json")[
        "supplemental"
    ]["numberingSystems"]
    properties["numbers.digits"] = systems.get(system, {}).get("_digits", DIGIT_DEFAULT)

    currencies = load(
        cluster / "cldr-numbers-full" / "main" / loc / "currencies.json"
    )["main"][loc]["numbers"]["currencies"]
    for code, entry in currencies.items():
        symbol = entry.get("symbol")
        if symbol and symbol != code:
            properties[f"currency.symbol.{code}"] = symbol

    return properties


def list_properties(cluster: Path, loc: str) -> dict[str, str]:
    patterns = load(cluster / "cldr-misc-full" / "main" / loc / "listPatterns.json")[
        "main"
    ][loc]["listPatterns"]["listPattern-type-standard"]
    return {
        "list.start": patterns.get("start", "{0}, {1}"),
        "list.middle": patterns.get("middle", "{0}, {1}"),
        "list.end": patterns.get("end", "{0}, {1}"),
        "list.two": patterns.get("2", "{0} and {1}"),
    }


def direction_of(cluster: Path, loc: str) -> str:
    orientation = load(cluster / "cldr-misc-full" / "main" / loc / "layout.json")[
        "main"
    ][loc]["layout"]["orientation"]["characterOrder"]
    return "rtl" if orientation == "right-to-left" else "ltr"


def locale_properties(cluster: Path, loc: str) -> dict[str, str]:
    properties = {
        "name": loc,
        "language": loc,
        "territory": "",
        "codeset": "UTF-8",
        "direction": direction_of(cluster, loc),
    }
    properties.update(number_properties(cluster, loc))
    properties.update(list_properties(cluster, loc))
    return properties


def serialize(properties: dict[str, str]) -> bytes:
    """The .locale file bytes: header, then key/value entries."""
    ordered = sorted(properties.items())
    out = bytearray(MAGIC)
    flags = FLAG_RTL if properties.get("direction") == "rtl" else 0
    out += flags.to_bytes(2, "little")
    out += (0).to_bytes(2, "little")
    out += len(ordered).to_bytes(4, "little")
    for key, value in ordered:
        key_bytes = key.encode("utf-8")
        value_bytes = value.encode("utf-8")
        out += len(key_bytes).to_bytes(2, "little")
        out += key_bytes
        out += len(value_bytes).to_bytes(4, "little")
        out += value_bytes
    return bytes(out)


def c_array(name: str, data: bytes) -> list[str]:
    lines = [f"const unsigned char {name}[] = {{"]
    for start in range(0, len(data), 16):
        chunk = data[start : start + 16]
        lines.append("    " + " ".join(f"0x{byte:02x}," for byte in chunk))
    lines.append("};")
    return lines


def emit(cluster: Path, manifest: Path, out_cc: Path, files_dir: Path | None = None) -> None:
    with manifest.open("rb") as handle:
        config = tomllib.load(handle)
    locales = config["locales"]
    default = config["default"]
    if default not in locales:
        raise SystemExit(f"default locale {default!r} is not in the locale list")

    blobs: list[tuple[str, bytes]] = []
    for loc in locales:
        blobs.append((loc, serialize(locale_properties(cluster, loc))))

    lines = [
        "/*",
        " * Generated by scripts/gen_locale_data.py from the pinned CLDR",
        f" * ({cluster.name}); do not edit.",
        " *",
        " * One byte array per locale in manifests/locales.toml and a lookup.",
        " * The bytes are the .locale format locale.cc parses (specs/locale.md).",
        " */",
        "",
        "#include \"locale_data.h\"",
        "",
        "namespace {",
    ]
    for index, (_loc, data) in enumerate(blobs):
        lines += c_array(f"kBlob{index}", data)
    lines.append("")
    lines.append("const aegir::trinket::detail::LocaleBlob kTable[] = {")
    for index, (loc, data) in enumerate(blobs):
        lines.append(f'    {{"{loc}", kBlob{index}, sizeof(kBlob{index})}},')
    lines.append("};")
    lines.append("")
    lines.append(f'constexpr char const kDefault[] = "{default}";')
    lines.append("}  // namespace")
    lines.append("")
    lines.append("namespace aegir::trinket::detail {")
    lines.append("")
    lines.append("LocaleBlob const *locale_blobs(unsigned int &count)")
    lines.append("{")
    lines.append("    count = sizeof(kTable) / sizeof(kTable[0]);")
    lines.append("    return kTable;")
    lines.append("}")
    lines.append("")
    lines.append("char const *locale_default_name()")
    lines.append("{")
    lines.append("    return kDefault;")
    lines.append("}")
    lines.append("")
    lines.append("}  // namespace aegir::trinket::detail")
    lines.append("")
    out_cc.write_text("\n".join(lines), encoding="utf-8")

    if files_dir is not None:
        files_dir.mkdir(parents=True, exist_ok=True)
        for loc, data in blobs:
            (files_dir / f"{loc}.locale").write_bytes(data)

    total = sum(len(data) for _loc, data in blobs)
    print(
        f"INFO  locale data: {len(blobs)} locales, {total} bytes of .locale tables",
        flush=True,
    )


def main(argv: list[str]) -> int:
    if len(argv) not in (3, 4):
        print(__doc__.strip())
        return 2
    files_dir = Path(argv[3]) if len(argv) == 4 else None
    emit(Path(argv[0]), Path(argv[1]), Path(argv[2]), files_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
