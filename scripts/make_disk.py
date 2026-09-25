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

from mkfs_bfs import bfs_minimum_bytes, make_bfs

SECTOR = 512
# The image's size is not a constant: it follows the partition table, which
# follows the command set (disk_bytes below, specs/dos.md).
# The Aegir system volume's partition type GUID (specs/services.md): the
# disk's own statement of which partition the system stands on, which the
# partition manager reads and the VFS aliases as Sys:. The discovery shape
# is systemd's Discoverable Partitions Specification -- one GUID per role.
AEGIR_SYSTEM_GUID = "5cd58811-9bf5-4af3-8682-9b76edce3535"
# The Be File System's GPT type (specs/bfs.md): the disk says which partition
# is BFS, and the filesystem registry turns that into the service that serves
# it.
BEFS_GUID = "42465331-3ba3-10f1-802a-4861696b7521"
# The partitions, in disk order. The AEGIR and BFS volumes are built by
# scripts/mkfs_bfs.py, so their contents live in the trees below and only the
# FAT partitions carry a known file. The first starts at the conventional LBA
# -- the first megabyte is the GPT's, which is also what real partitioning
# tools leave. A size of None is computed: AEGIR's from its tree, because
# Sys:C holds the command set and the set grows (specs/dos.md). Every start
# follows from the partition before it, so a bigger AEGIR shifts the rest and
# the image with them.
PARTITION_LAYOUT = [
    ("AEGIR", None, "AEGIR.TXT",
     b"aegir read this file off a disk it enumerated itself\n"),
    ("SECOND", 8192, "SECOND.TXT",
     b"a second volume, a second service, the same reader\n"),
    # No file: the writable volume the write side proves itself on. An empty
    # root directory is the point -- everything in it, the system put there.
    ("SCRATCH", 6143, None, None),
    # FAT16, and provably so: with one sector per cluster this many sectors
    # is past the 4085-cluster FAT12 ceiling and under FAT32's floor, which
    # is the format's own definition of the flavor. mtools picks from the
    # geometry, so the minfo check below is the checksum, not a courtesy.
    ("FAT16", 10240, None, None),
    # The Be File System: Aegir's own. mkfs_bfs makes a root with a known
    # file and a directory with a nested file, so the read half has something
    # to walk.
    ("BFS", 20480, None, None),
]

# The system volume's tree (specs/services.md, specs/bfs.md): the migration to
# BFS, so what Sys: holds is what the file permissions arc can own. The names
# are the ones the FAT build used, long names included, so the readers see the
# same disk by a different filesystem.
AEGIR_BFS_TREE = [
    ("file", "AEGIR.TXT", b"aegir read this file off a disk it enumerated itself\n"),
    # A file longer than any window, so `more` has to page it (specs/dos.md).
    ("file", "LONG.TXT", b"".join(b"page line %d\n" % n for n in range(1, 61))),
    # A version string for `version` to find (specs/dos.md).
    ("file", "VER.TXT", b"$VER: VER.TXT 1.0 (25.9.2026) an Aegir test fixture\n"),
    ("dir", "DOCS", [
        ("file", "NESTED.TXT",
         b"two components deep, and the walk found it\n"),
    ]),
    ("file", "Readme With A Long Name.txt",
     b"a long name, read back whole -- the 8.3 form cannot spell it\n"),
    ("dir", "A Long Folder", [
        ("file", "Inside Long Name.txt",
         b"a long directory, a long file, and the walk still ends at it\n"),
    ]),
    # The system's environment archive (specs/environment.md): the base member
    # of the ENV: union. A session's startup reads it into its environment, so
    # the shell knows exitcode before anything sets it.
    ("dir", "Prefs", [
        ("dir", "Env-Archive", [
            ("file", "exitcode", b"11"),
        ]),
    ]),
]

# The BFS volume's tree: a known file and a directory with a nested file, so
# the component walk crosses a directory the way the FAT one does.
BFS_TREE = [
    ("file", "HELLO.TXT", b"a Be File System file, read off a disk Aegir built itself\n"),
    ("dir", "SUBDIR", [
        ("file", "NESTED.TXT",
         b"two components deep, through a Be File System directory\n"),
    ]),
]


def aegir_tree(commands) -> list:
    """The system volume's tree, with the command set as Sys:C.

    `commands` is a list of (name, bytes): each becomes C/<name>, the flat
    lowercase command name the shell resolves (specs/dos.md)."""
    tree = list(AEGIR_BFS_TREE)
    if commands:
        tree.append(("dir", "C", [("file", name, data) for name, data in commands]))
    return tree


def partition_table(commands) -> list:
    """Lay the partitions out from their contents.

    AEGIR's size is its tree's, computed (specs/dos.md); every start is the
    previous partition's end rounded up to a whole megabyte, so a bigger
    command set moves what follows and the image grows with it. Rows are the
    shape the rest of this file uses: (name, first LBA, last LBA, known file,
    known content)."""
    cursor = 2048
    rows = []
    for name, sectors, known_name, known_content in PARTITION_LAYOUT:
        first = ((cursor + 2047) // 2048) * 2048
        if sectors is None:
            needed = bfs_minimum_bytes(aegir_tree(commands), name)
            sectors = (needed + SECTOR - 1) // SECTOR
            sectors = ((sectors + 2047) // 2048) * 2048
        last = first + sectors - 1
        rows.append((name, first, last, known_name, known_content))
        cursor = last + 1
    return rows


def disk_bytes(partitions) -> int:
    # A whole megabyte past the last partition too: the backup GPT lives at
    # the end of the image, and sgdisk refuses a partition that reaches it.
    end = max(last for _, _, last, _, _ in partitions) + 1 + 2048
    return ((end + 2047) // 2048) * 2048 * SECTOR

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
    parser.add_argument(
        "--commands", type=Path, default=None,
        help="a directory of command images, one per file, packed as Sys:C "
             "(specs/dos.md); the AEGIR partition is sized from them")
    args = parser.parse_args()

    commands = []
    if args.commands is not None and args.commands.is_dir():
        commands = [(path.name, path.read_bytes())
                    for path in sorted(args.commands.iterdir()) if path.is_file()]
    partitions = partition_table(commands)
    total_bytes = disk_bytes(partitions)

    for tool in ("sgdisk", "mformat", "mcopy", "mdir"):
        if shutil.which(tool) is None:
            print(f"make_disk: {tool} is not on PATH (gdisk and mtools)", file=sys.stderr)
            return 1

    with args.image.open("wb") as handle:
        handle.truncate(total_bytes)

    # Three partitions. The first carries the Aegir system volume's type
    # GUID -- the disk says which volume is Sys: -- the rest are plain
    # Microsoft basic data, the type a FAT volume on GPT carries.
    sgdisk = ["sgdisk", "--clear"]
    for number, (name, first, last, _, _) in enumerate(partitions, start=1):
        if number == 1:
            typecode = AEGIR_SYSTEM_GUID
        elif name == "BFS":
            typecode = BEFS_GUID
        else:
            typecode = "0700"
        sgdisk += [f"--new={number}:{first}:{last if last is not None else 0}",
                   f"--typecode={number}:{typecode}",
                   f"--change-name={number}:{name}"]
    sgdisk.append(str(args.image))
    subprocess.run(sgdisk, check=True, capture_output=True)

    for name, first, _, known_name, known_content in partitions:
        if name in ("AEGIR", "BFS"):
            # Not mtools': AEGIR is the system volume and BFS the second BFS
            # volume, both built by scripts/mkfs_bfs.py below, so their
            # on-disk format is the one specs/bfs.md fixes.
            continue
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
        if name == "AEGIR":
            continue  # the system volume is BFS now; its tree is AEGIR_BFS_TREE
        first = next(p[1] for p in partitions if p[0] == name)
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
        if name == "AEGIR":
            continue  # the system volume is BFS now; its tree is AEGIR_BFS_TREE
        first = next(p[1] for p in partitions if p[0] == name)
        volume = f"{args.image}@@{first * SECTOR}"
        with tempfile.TemporaryDirectory() as staging:
            source = Path(staging) / long_name
            source.write_bytes(long_content)
            subprocess.run(
                ["mcopy", "-i", volume, str(source), f"::{long_name}"],
                check=True, capture_output=True)

    for name, directory, long_name, long_content in LONG_NESTED:
        if name == "AEGIR":
            continue  # the system volume is BFS now; its tree is AEGIR_BFS_TREE
        first = next(p[1] for p in partitions if p[0] == name)
        volume = f"{args.image}@@{first * SECTOR}"
        subprocess.run(["mmd", "-i", volume, f"::{directory}"], check=True,
                       capture_output=True)
        with tempfile.TemporaryDirectory() as staging:
            source = Path(staging) / long_name
            source.write_bytes(long_content)
            subprocess.run(
                ["mcopy", "-i", volume, str(source), f"::{directory}/{long_name}"],
                check=True, capture_output=True)

    # The two BFS volumes -- the system volume and the second one: built into
    # the image at their partitions' offsets, the same way mtools fills a FAT
    # partition through `image@@offset`.
    with args.image.open("r+b") as handle:
        handle.seek(0)
        image = bytearray(handle.read())
        for name, tree in (("AEGIR", aegir_tree(commands)), ("BFS", BFS_TREE)):
            first = next(p[1] for p in partitions if p[0] == name)
            last = next(p[2] for p in partitions if p[0] == name)
            make_bfs(image, first * SECTOR, (last - first + 1) * SECTOR, name, tree)
        handle.seek(0)
        handle.write(image)

    print(f"make_disk: {args.image}: GPT, three FAT partitions and two BFS "
          "(the system volume among them), known files, nested and long names, "
          "one empty volume, one FAT16")
    if commands:
        print(f"make_disk: Sys:C holds {len(commands)} commands, the AEGIR "
              f"partition sized from them")
    return 0


if __name__ == "__main__":
    sys.exit(main())
