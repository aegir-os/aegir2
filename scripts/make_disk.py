#!/usr/bin/env python3
"""Build the test disk: a GPT with one FAT partition holding one known file.

Copyright (c) 2026 Robert Roland
SPDX-License-Identifier: MIT

The partition manager and the filesystem service need a disk worth reading:
a GPT that names a partition, and a FAT volume in it whose root directory
lists a file with content we can check for. Building one needs no loop
device and no root -- sgdisk writes the GPT into the file directly, and
mtools works on a partition *inside* an image with its `image@@offset`
syntax -- so the whole thing is three tool invocations and a truncate.

The image is created once and left alone (scripts/run_target.py): a disk
that changes between runs is not something to depend on. Delete it to make
a fresh one.
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SECTOR = 512
DISK_BYTES = 16 << 20
# The partition starts at the conventional LBA: the first megabyte is the
# GPT's, which is also what real partitioning tools leave.
FIRST_LBA = 2048
# What the filesystem service will be asked to find. The content is the
# checksum: a reader that got the wrong sectors does not print this.
KNOWN_NAME = "AEGIR.TXT"
KNOWN_CONTENT = b"aegir read this file off a disk it enumerated itself\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="the disk image to create")
    args = parser.parse_args()

    for tool in ("sgdisk", "mformat", "mcopy", "mdir"):
        if shutil.which(tool) is None:
            print(f"make_disk: {tool} is not on PATH (gdisk and mtools)", file=sys.stderr)
            return 1

    with args.image.open("wb") as handle:
        handle.truncate(DISK_BYTES)

    # One partition covering the rest of the disk, typed Microsoft basic
    # data -- the type a FAT volume on GPT carries.
    subprocess.run(
        [
            "sgdisk",
            "--clear",
            f"--new=1:{FIRST_LBA}:0",
            "--typecode=1:0700",
            "--change-name=1:AEGIR",
            str(args.image),
        ],
        check=True,
        capture_output=True,
    )

    offset = FIRST_LBA * SECTOR
    volume = f"{args.image}@@{offset}"
    # -F forces FAT32, which a volume this size would not normally get; the
    # service's first reader is meant to speak it. If this mtools refuses,
    # the geometry default -- FAT16 at this size -- still exercises the
    # same BPB walk, so either answer is a disk worth having.
    forced = subprocess.run(
        ["mformat", "-i", volume, "-F", "-v", "AEGIR", "::"],
        capture_output=True,
    )
    if forced.returncode != 0:
        subprocess.run(
            ["mformat", "-i", volume, "-v", "AEGIR", "::"],
            check=True,
            capture_output=True,
        )

    with tempfile.TemporaryDirectory() as staging:
        source = Path(staging) / KNOWN_NAME
        source.write_bytes(KNOWN_CONTENT)
        subprocess.run(
            ["mcopy", "-i", volume, str(source), f"::{KNOWN_NAME}"],
            check=True,
            capture_output=True,
        )

    listing = subprocess.run(
        ["mdir", "-i", volume, "-/", "::"], check=True, capture_output=True, text=True
    )
    # mdir pads an 8.3 name out with spaces ("AEGIR    TXT"), so compare with
    # every separator removed from both sides.
    if KNOWN_NAME.replace(".", "") not in "".join(listing.stdout.split()):
        print(f"make_disk: the file did not land:\n{listing.stdout}", file=sys.stderr)
        return 1
    print(f"make_disk: {args.image}: GPT, one FAT partition, {KNOWN_NAME} in its root")
    return 0


if __name__ == "__main__":
    sys.exit(main())
