#!/usr/bin/env python3
"""Build the test disk: a GPT with three FAT partitions -- two holding a known
file, one empty.

Copyright (c) 2026 Robert Roland
SPDX-License-Identifier: MIT

The partition manager and the filesystem service need a disk worth reading:
a GPT that names partitions, and a FAT volume in each whose root directory
lists a file with content we can check for. Two partitions with files, not
one, because the second is the proof that the range grant works: its BPB is
nowhere near sector 0, and its service reads its own file out of its own
window set. The third is empty and writable -- the write side's proving
ground, where everything in the root directory is something the system put
there. Building one needs no loop device and no root -- sgdisk writes the GPT
into the file directly, and mtools works on a partition *inside* an image
with its `image@@offset` syntax -- so the whole thing is tool invocations
and a truncate.

The image is created once and left alone (scripts/run_target.py), and QEMU
takes it read-only with `-snapshot`, so a run's writes land in a throwaway
overlay: a disk that changes between runs is not something to depend on.
Delete it to make a fresh one.
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SECTOR = 512
DISK_BYTES = 16 << 20
# The partitions: name, first LBA, last LBA (None = to the end of the disk),
# and the file each volume's root will hold. The first starts at the
# conventional LBA -- the first megabyte is the GPT's, which is also what
# real partitioning tools leave. What the filesystem services will be asked
# to find: the contents are the checksum, a reader that got the wrong
# sectors does not print them.
PARTITIONS = [
    ("AEGIR", 2048, 18431, "AEGIR.TXT",
     b"aegir read this file off a disk it enumerated itself\n"),
    ("SECOND", 18432, 26623, "SECOND.TXT",
     b"a second volume, a second service, the same reader\n"),
    # No file: the writable volume the write side proves itself on. An empty
    # root directory is the point -- everything in it, the system put there.
    ("SCRATCH", 26624, None, None, None),
]


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

    # Three partitions typed Microsoft basic data -- the type a FAT volume on
    # GPT carries.
    sgdisk = ["sgdisk", "--clear"]
    for number, (name, first, last, _, _) in enumerate(PARTITIONS, start=1):
        sgdisk += [f"--new={number}:{first}:{last if last is not None else 0}",
                   f"--typecode={number}:0700",
                   f"--change-name={number}:{name}"]
    sgdisk.append(str(args.image))
    subprocess.run(sgdisk, check=True, capture_output=True)

    for name, first, _, known_name, known_content in PARTITIONS:
        volume = f"{args.image}@@{first * SECTOR}"
        # -F forces FAT32, which a volume this size would not normally get;
        # the service's first reader is meant to speak it. If this mtools
        # refuses, the geometry default -- FAT16 at this size -- still
        # exercises the same BPB walk, so either answer is a disk worth
        # having.
        forced = subprocess.run(
            ["mformat", "-i", volume, "-F", "-v", name, "::"],
            capture_output=True,
        )
        if forced.returncode != 0:
            subprocess.run(
                ["mformat", "-i", volume, "-v", name, "::"],
                check=True,
                capture_output=True,
            )

        if known_name is None:
            continue
        with tempfile.TemporaryDirectory() as staging:
            source = Path(staging) / known_name
            source.write_bytes(known_content)
            subprocess.run(
                ["mcopy", "-i", volume, str(source), f"::{known_name}"],
                check=True,
                capture_output=True,
            )

        listing = subprocess.run(
            ["mdir", "-i", volume, "-/", "::"], check=True, capture_output=True, text=True
        )
        # mdir pads an 8.3 name out with spaces ("AEGIR    TXT"), so compare
        # with every separator removed from both sides.
        if known_name.replace(".", "") not in "".join(listing.stdout.split()):
            print(f"make_disk: the file did not land:\n{listing.stdout}", file=sys.stderr)
            return 1
    print(f"make_disk: {args.image}: GPT, three FAT partitions, "
          "two known files and one empty volume")
    return 0


if __name__ == "__main__":
    sys.exit(main())
