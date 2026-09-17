#!/usr/bin/env python3
"""Pack the user database: descriptor rows for people, a binary table for
the service (specs/auth.md).

Usage: make_users.py <source> <output>

The source is one row per user, `name=.. secret=.. account=..` (account
defaults to the name), `#` comments and blank lines skipped. The output is
the table auth parses directly: a 16-byte header, then 80-byte rows of
NUL-terminated fields.
"""

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"AUDB"
VERSION = 1
NAME_BYTES = 24
ACCOUNT_BYTES = 24
SECRET_BYTES = 32


def field(text: str, width: int, what: str, line: int) -> bytes:
    raw = text.encode()
    if len(raw) >= width:
        raise ValueError(
            f"line {line}: {what} is {len(raw)} bytes; the field holds {width - 1}"
        )
    return raw.ljust(width, b"\0")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="the descriptor-row user list")
    parser.add_argument("output", type=Path, help="the packed table to write")
    args = parser.parse_args()

    rows = []
    for number, line in enumerate(args.source.read_text().splitlines(), start=1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = {}
        for word in line.split():
            key, _, value = word.partition("=")
            fields[key] = value
        if "name" not in fields or "secret" not in fields:
            print(f"make_users: line {number}: a row needs name= and secret=", file=sys.stderr)
            return 1
        account = fields.get("account", fields["name"])
        try:
            rows.append(
                field(fields["name"], NAME_BYTES, "the name", number)
                + field(account, ACCOUNT_BYTES, "the account", number)
                + field(fields["secret"], SECRET_BYTES, "the secret", number)
            )
        except ValueError as problem:
            print(f"make_users: {problem}", file=sys.stderr)
            return 1

    table = struct.pack("<4sIII", MAGIC, VERSION, len(rows), 0) + b"".join(rows)
    args.output.write_bytes(table)
    print(f"make_users: {args.output}: {len(rows)} user(s), {len(table)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
