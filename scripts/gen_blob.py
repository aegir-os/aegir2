#!/usr/bin/env python3
"""Write a large file of a known pattern, for the scale acceptance.

The pattern is the point: a blob of zeros would pass even if the loader never
filled the segment, because a zero page and an unfilled page read the same.
`i & 0xff` does not, so a checksum over the loaded bytes fails loudly when they
were not loaded -- which is what makes this a test of the loader and not only of
its bookkeeping.

    python3 scripts/gen_blob.py <path> <mib>

Idempotent: a file already the right size is left alone, so a rebuild does not
rewrite 32 MiB every time.
"""

from __future__ import annotations

import sys
from pathlib import Path


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.splitlines()[0], file=sys.stderr)
        print("usage: gen_blob.py <path> <mib>", file=sys.stderr)
        return 2
    path = Path(argv[1])
    size = int(argv[2]) * 1024 * 1024
    if path.is_file() and path.stat().st_size == size:
        return 0
    path.parent.mkdir(parents=True, exist_ok=True)
    block = bytes(i & 0xFF for i in range(4096))
    with open(path, "wb") as out:
        written = 0
        while written < size:
            chunk = block[: min(len(block), size - written)]
            out.write(chunk)
            written += len(chunk)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
