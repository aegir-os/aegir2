#!/usr/bin/env python3
"""Compile a gettext .po catalogue into an embedded .mo image.

    python3 scripts/compile_po.py <catalogue.po> <out.cc>

A small msgfmt: it reads the subset of the .po format the toolkit's catalogues
use (msgctxt, msgid, msgid_plural, msgstr, msgstr[N], comments, and quoted
continuation lines with the common escapes), assembles a GNU .mo image in
little-endian byte order, and writes it as a C++ source exposing it as
`aegir::trinket::detail::translation_blob()`.

The system case -- a catalogue read from a volume -- is deferred, so the
toolkit embeds one (specs/locale.md). xgettext extracts the template; this is
the compiler half, and the built binary is what `Translation` parses.

Stdlib only, so the host needs nothing but python3.
"""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

MAGIC = 0x950412DE


def unescape(inner: str) -> str:
    out = []
    i = 0
    escapes = {
        "n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\",
        "a": "\a", "b": "\b", "f": "\f", "v": "\v", "0": "\0",
    }
    while i < len(inner):
        c = inner[i]
        if c == "\\" and i + 1 < len(inner):
            n = inner[i + 1]
            out.append(escapes.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def decode(rest: str) -> str:
    rest = rest.strip()
    if len(rest) >= 2 and rest.startswith('"') and rest.endswith('"'):
        rest = rest[1:-1]
    return unescape(rest)


KEYWORD = re.compile(r"^(msgid_plural|msgctxt|msgid|msgstr)(?:\[(\d+)\])?\s*(.*)$")


def parse_po(text: str) -> list[dict]:
    entries: list[dict] = []
    entry: dict | None = None
    last: tuple[str, int | None] | None = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            if entry is not None:
                entries.append(entry)
                entry = None
            last = None
            continue
        if line.startswith("#"):
            continue
        match = KEYWORD.match(line)
        if match is not None:
            if entry is None:
                entry = {}
            key, index, rest = match.group(1), match.group(2), match.group(3)
            value = decode(rest)
            if key == "msgstr":
                if index is not None:
                    entry.setdefault("forms", {})[int(index)] = value
                    last = ("form", int(index))
                else:
                    entry["msgstr"] = value
                    last = ("msgstr", None)
            else:
                entry.setdefault(key, "")
                entry[key] += value
                last = (key, None)
        elif line.startswith('"'):
            if entry is None or last is None:
                continue
            value = decode(line)
            key, index = last
            if key == "msgstr":
                entry["msgstr"] += value
            elif key == "form":
                entry["forms"][index] += value
            else:
                entry[key] += value
        else:
            raise SystemExit(f"compile_po: cannot parse line: {raw!r}")
    if entry is not None:
        entries.append(entry)
    return entries


def original_of(entry: dict) -> str:
    msgid = entry.get("msgid", "")
    context = entry.get("msgctxt")
    plural = entry.get("msgid_plural")
    if context is not None:
        return context + "\x04" + msgid + ("\x00" + plural if plural is not None else "")
    if plural is not None:
        return msgid + "\x00" + plural
    return msgid


def translated_of(entry: dict) -> str:
    forms = entry.get("forms")
    if forms is not None:
        return "\x00".join(forms[index] for index in sorted(forms))
    return entry.get("msgstr", "")


def build_mo(pairs: list[tuple[str, str]]) -> bytes:
    """The GNU .mo image: header, two descriptor tables, then the strings."""
    pairs = sorted(pairs, key=lambda pair: pair[0].encode("utf-8"))
    count = len(pairs)
    originals_table = 28
    translations_table = originals_table + 8 * count
    originals_at = translations_table + 8 * count
    translations_at = originals_at + sum(len(k.encode("utf-8")) for k, _ in pairs)

    header = struct.pack("<7I", MAGIC, 0, count, originals_table, translations_table, 0, 0)
    original_offsets = []
    cursor = originals_at
    for key, _ in pairs:
        original_offsets.append(cursor)
        cursor += len(key.encode("utf-8"))
    translation_offsets = []
    cursor = translations_at
    for _, value in pairs:
        translation_offsets.append(cursor)
        cursor += len(value.encode("utf-8"))

    out = bytearray(header)
    for (key, _), offset in zip(pairs, original_offsets):
        out += struct.pack("<2I", len(key.encode("utf-8")), offset)
    for (_, value), offset in zip(pairs, translation_offsets):
        out += struct.pack("<2I", len(value.encode("utf-8")), offset)
    for key, _ in pairs:
        out += key.encode("utf-8")
    for _, value in pairs:
        out += value.encode("utf-8")
    return bytes(out)


def c_array(name: str, data: bytes) -> list[str]:
    lines = [f"const unsigned char {name}[] = {{"]
    for start in range(0, len(data), 16):
        chunk = data[start : start + 16]
        lines.append("    " + " ".join(f"0x{byte:02x}," for byte in chunk))
    lines.append("};")
    return lines


def emit(po: Path, out_cc: Path) -> None:
    entries = parse_po(po.read_text(encoding="utf-8"))
    pairs = [(original_of(entry), translated_of(entry)) for entry in entries]
    if not pairs or pairs[0][0] != "":
        raise SystemExit("compile_po: the catalogue has no header entry")
    image = build_mo(pairs)

    lines = [
        "/*",
        " * Generated by scripts/compile_po.py from",
        f" * {po.name}; do not edit.",
        " *",
        " * The .mo image is embedded so the toolkit carries its built-in",
        " * catalogue without a file system (specs/locale.md).",
        " */",
        "",
        '#include "translation_data.h"',
        "",
        "namespace {",
    ]
    lines += c_array("kCatalogue", image)
    lines.append("}  // namespace")
    lines.append("")
    lines.append("namespace aegir::trinket::detail {")
    lines.append("")
    lines.append("std::string_view translation_blob()")
    lines.append("{")
    lines.append('    return std::string_view(reinterpret_cast<char const *>(kCatalogue), sizeof(kCatalogue));')
    lines.append("}")
    lines.append("")
    lines.append("}  // namespace aegir::trinket::detail")
    lines.append("")
    out_cc.write_text("\n".join(lines), encoding="utf-8")
    print(f"INFO  catalogue: {len(pairs)} messages, {len(image)} bytes of .mo", flush=True)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip())
        return 2
    emit(Path(argv[0]), Path(argv[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
