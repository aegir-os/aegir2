#!/usr/bin/env python3
"""Build the test disk: a GPT with four FAT partitions -- two holding a known
file, one empty for the write side, one FAT16 for the flavors to differ.

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
DISK_BYTES = 32 << 20
# The Aegir system volume's partition type GUID (specs/services.md): the
# disk's own statement of which partition the system stands on, which the
# partition manager reads and the VFS aliases as Sys:. The discovery shape
# is systemd's Discoverable Partitions Specification -- one GUID per role.
AEGIR_SYSTEM_GUID = "5cd58811-9bf5-4af3-8682-9b76edce3535"
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
    ("SCRATCH", 26624, 32766, None, None),
    # FAT16, and provably so: with one sector per cluster this many sectors
    # is past the 4085-cluster FAT12 ceiling and under FAT32's floor, which
    # is the format's own definition of the flavor. mtools picks from the
    # geometry, so the minfo check below is the checksum, not a courtesy.
    ("FAT16", 32768, 43007, None, None),
]

# Beyond the root files: a directory with a file in it, so the component
# walk has something to find (specs/vfs.md). (volume name, directory, file,
# content -- the content is the checksum again.)
NESTED = [
    ("AEGIR", "DOCS", "NESTED.TXT",
     b"two components deep, and the walk found it\n"),
]

# Long names, the VFAT layer the 8.3 form cannot spell (specs/fat.md): one in
# a root, and one that names a directory so a component walk crosses a long
# name too. mtools writes each as a long-name run plus a generated 8.3 alias;
# the reader must match the long form, not only the alias.
# (volume, name, content).
LONG_ROOT = [
    ("AEGIR", "Readme With A Long Name.txt",
     b"a long name, read back whole -- the 8.3 form cannot spell it\n"),
]
LONG_NESTED = [
    ("AEGIR", "A Long Folder", "Inside Long Name.txt",
     b"a long directory, a long file, and the walk still ends at it\n"),
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

    # Three partitions. The first carries the Aegir system volume's type
    # GUID -- the disk says which volume is Sys: -- the rest are plain
    # Microsoft basic data, the type a FAT volume on GPT carries.
    sgdisk = ["sgdisk", "--clear"]
    for number, (name, first, last, _, _) in enumerate(PARTITIONS, start=1):
        typecode = AEGIR_SYSTEM_GUID if number == 1 else "0700"
        sgdisk += [f"--new={number}:{first}:{last if last is not None else 0}",
                   f"--typecode={number}:{typecode}",
                   f"--change-name={number}:{name}"]
    sgdisk.append(str(args.image))
    subprocess.run(sgdisk, check=True, capture_output=True)

    for name, first, _, known_name, known_content in PARTITIONS:
        volume = f"{args.image}@@{first * SECTOR}"
        if name == "FAT16":
            # FAT16 on purpose: -c 1 makes the cluster count decide, and the
            # count is FAT16's range. mtools picks by geometry, so verify --
            # a build that silently made FAT12 would feed the service a
            # flavor it does not speak.
            subprocess.run(
                ["mformat", "-i", volume, "-c", "1", "-v", name, "::"],
                check=True, capture_output=True,
            )
            info = subprocess.run(["minfo", "-i", volume, "::"], check=True,
                                  capture_output=True, text=True)
            if 'disk type="FAT16' not in info.stdout:
                print(f"make_disk: the FAT16 partition is not one:\n{info.stdout}",
                      file=sys.stderr)
                return 1
        else:
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

    for name, directory, nested_name, nested_content in NESTED:
        first = next(p[1] for p in PARTITIONS if p[0] == name)
        volume = f"{args.image}@@{first * SECTOR}"
        subprocess.run(["mmd", "-i", volume, f"::{directory}"], check=True,
                       capture_output=True)
        with tempfile.TemporaryDirectory() as staging:
            source = Path(staging) / nested_name
            source.write_bytes(nested_content)
            subprocess.run(
                ["mcopy", "-i", volume, str(source), f"::{directory}/{nested_name}"],
                check=True, capture_output=True)
        listing = subprocess.run(
            ["mdir", "-i", volume, "-/", "::"], check=True, capture_output=True, text=True
        )
        if nested_name.replace(".", "") not in "".join(listing.stdout.split()):
            print(f"make_disk: the nested file did not land:\n{listing.stdout}",
                  file=sys.stderr)
            return 1

    for name, long_name, long_content in LONG_ROOT:
        first = next(p[1] for p in PARTITIONS if p[0] == name)
        volume = f"{args.image}@@{first * SECTOR}"
        with tempfile.TemporaryDirectory() as staging:
            source = Path(staging) / long_name
            source.write_bytes(long_content)
            subprocess.run(
                ["mcopy", "-i", volume, str(source), f"::{long_name}"],
                check=True, capture_output=True)

    for name, directory, long_name, long_content in LONG_NESTED:
        first = next(p[1] for p in PARTITIONS if p[0] == name)
        volume = f"{args.image}@@{first * SECTOR}"
        subprocess.run(["mmd", "-i", volume, f"::{directory}"], check=True,
                       capture_output=True)
        with tempfile.TemporaryDirectory() as staging:
            source = Path(staging) / long_name
            source.write_bytes(long_content)
            subprocess.run(
                ["mcopy", "-i", volume, str(source), f"::{directory}/{long_name}"],
                check=True, capture_output=True)

    print(f"make_disk: {args.image}: GPT, four FAT partitions, "
          "two known files, one nested, long names, one empty volume, one FAT16")
    return 0


if __name__ == "__main__":
    sys.exit(main())
