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

The image is created once and left alone (scripts/run_target.py), and a run
answers its writes through a throwaway overlay the runner makes over it, so a
disk that changes between runs is not something to depend on.
Delete it to make a fresh one.
"""

import argparse
import base64
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
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

def make_ilbm() -> bytes:
    """A tiny uncompressed ILBM fixture, read through the ilbm class.

    The demo's datatypes acceptance (specs/datatypes.md) opens this through
    the `ilbm.datatype` class, so the image is built here rather than vendored:
    what is pinned is the client and the class, not a byte blob. One bit plane,
    two CMAP entries (a red left half, a green right half), no mask and no
    ByteRun1 -- the decoder's plainest path.
    """
    width, height = 64, 48
    # One plane, rows whole 16-bit words: 8 bytes a row, x 0..7 in byte 0 and
    # x 32..39 in byte 4, so the low x's are zero and the high x's are set.
    body = b"\x00\x00\x00\x00\xff\xff\xff\xff" * height
    bmhd = struct.pack(">HHhhBBBBHBBhh", width, height, 0, 0, 1, 0, 0, 0, 0,
                       1, 1, width, height)
    cmap = bytes([255, 0, 0, 0, 255, 0])

    def chunk(cid: bytes, data: bytes) -> bytes:
        return cid + struct.pack(">I", len(data)) + data + (b"\x00" if len(data) & 1 else b"")

    payload = (b"ILBM" + chunk(b"BMHD", bmhd) + chunk(b"CMAP", cmap) +
               chunk(b"BODY", body))
    return b"FORM" + struct.pack(">I", len(payload)) + payload


def make_png() -> bytes:
    """A tiny PNG fixture, read through the png class.

    The second datatype acceptance (specs/datatypes.md): the demo opens this
    through the `png.datatype` class, so the image is built here. 8-bit RGBA,
    no interlace, one IDAT -- a blue left half and a white right half. The
    colours differ from make_ilbm's on purpose: the acceptance pins a pixel in
    each image, so a rect that named the wrong one, or a decode that mixed the
    channels, shows a colour the other class never produces. zlib's compress is
    the stdlib's, used only to build the file, not to decode it.
    """
    width, height = 64, 48
    raw = bytearray()
    for _y in range(height):
        raw.append(0)  # filter: none
        for x in range(width):
            raw += bytes((0, 0, 255, 255) if x < width // 2 else (255, 255, 255, 255))

    def chunk(cid: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + cid + data +
                struct.pack(">I", zlib.crc32(cid + data) & 0xffffffff))

    # IHDR: width, height, bit depth 8, colour type 6 (RGBA), no interlace.
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def make_jpeg() -> bytes:
    """A tiny JPEG fixture, read through the jpeg class.

    The third datatype acceptance (specs/datatypes.md): the demo opens this
    through the `jpeg.datatype` class, so the image is built here -- except that
    it is not: the standard library has no JPEG encoder, so the bytes are
    embedded. They were written at quality 100, 4:4:4, by an encoder other than
    the one under test, so the class decodes a foreign file. 64x48, an orange
    left half and a purple right half; the pinned libjpeg-turbo decodes those
    halves to (255,127,0) and (127,0,255), which is what the acceptance pins --
    the colours differ from make_ilbm's and make_png's, so a rect that named the
    wrong image shows.
    """
    encoded = (
        b"/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB"
        b"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/2wBDAQEBAQEBAQEBAQEBAQEBAQEBAQEB"
        b"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/wAARCAAwAEADAREA"
        b"AhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQA"
        b"AAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3"
        b"ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWm"
        b"p6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEA"
        b"AwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSEx"
        b"BhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElK"
        b"U1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3"
        b"uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD64r/l"
        b"XP8ApQCgAoAKAP5X6/8AX4P4HCgAoAKAP6oK/wDIHP74CgAoAKAP5X6/9fg/gcKACgAoA/qgr/yB"
        b"z++AoAKACgD+V+v/AF+D+BwoAKACgD+qCv8AyBz++AoAKACgD+V+v/X4P4HCgAoAKAP6oK/8gc/v"
        b"gKACgAoA/lfr/wBfg/gcKACgAoA/qgr/AMgc/vgKACgAoA/lfr/1+D+BwoAKACgD/9k="
    )
    return base64.b64decode(encoded)


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
    # The system's script directory (specs/shell.md, specs/boot.md): the
    # startup files the shell and the boot session read. The user's own S: is
    # a home directory made at login; nothing unions the two, so the system's
    # scripts never show in the user's listing.
    ("dir", "S", [
        # Every interactive shell runs this when the user's Home:S/Shell-Startup
        # is absent (specs/shell.md). `Run` starts the window manager's demo
        # client in the session (specs/window-manager.md): it is a command now,
        # not a boot service, so it holds the launch.session caller half and can
        # serve-launch a datatype class (specs/datatypes.md). The demo waits for
        # the bureau before registering its menus, so starting it here -- beside
        # the terminal, synchronously with the interactive loop -- is safe.
        ("file", "Shell-Startup", b"alias l list\nRun gui-demo\n"),
        # The **D** flag (specs/process.md Phase 4): the frame's first line breaks
        # the shell itself with D, so the shell halts the frame and the second
        # line never runs.
        ("file", "CtrlD-Test",
         b"break x name session.shell d\naegir-echo CtrlD-not-reached\n"),
        # The enforced halt (specs/process.md Phase 3): a background `wait` -- a
        # long timer that never returns and never polls its break source -- is
        # Broken and reaped, then Control-Test runs. It is a file, not three
        # console lines, because the `wait` image starts between the first and
        # second: typed from the console, the `break` line lands while that
        # image is starting and the guest's key queue drops it (run_target's
        # press; the same reason the command files are typed one per step).
        ("file", "Halt-Test",
         b"run wait 99999\nbreak x name wait\nexecute Sys:S/Control-Test\n"),
        
        # The session's services, as data (specs/session.md): auth reads the
        # user's Home:S/session.manifest first, then this. `authority` is always
        # user -- a session service runs as the session's user class, never the
        # system's, and a `system` authority here is a parse error. The bureau
        # and a terminal are what a session is today; the terminal is
        # convenience while there is no desktop, and this file is where that
        # stops being code in auth.
        ("file", "session.manifest",
         b"# Aegir's session manifest. See specs/session.md for the format and\n"
         b"# the rules auth validates against. authority is always `user`.\n"
         b"format = 1\n"
         b"\n"
         b"[bureau]\n"
         b"binary     = aegir-bureau\n"
         b"authority  = user\n"
         b"account    = user\n"
         b"owns       = bureau.menu\n"
         b"needs      = log.main, vfs.namespace, console.gui, font.main, launch.session\n"
         b"maps       = true\n"
         b"memory_kib = 1024\n"
         b"\n"
         b"[terminal]\n"
         b"binary      = aegir-terminal\n"
         b"authority   = user\n"
         b"account     = user\n"
         b"needs       = log.main, vfs.namespace, console.gui, clock.main, timer.main, mem.main, font.main, process.registry, launch.session\n"
         b"maps        = true\n"
         b"memory_kib  = 4096\n"
         b"cspace_bits = 13\n"
         b"delegate_mib = 4\n"
         b"\n"
         b"# The session's datatypes broker (specs/datatypes.md): it owns\n"
         b"# datatypes.main, so a program asks it to open a file rather than starting\n"
         b"# a class itself, and it starts the class through the session launcher as\n"
         b"# the session's user class.\n"
         b"[datatypes]\n"
         b"binary     = aegir-datatypes-broker\n"
         b"authority  = user\n"
         b"account    = user\n"
         b"owns       = datatypes.main\n"
         b"needs      = log.main, vfs.namespace, launch.session\n"
         b"maps       = true\n"
         b"memory_kib = 4096\n"),
        # Names the resolver knows without DNS (specs/net.md): a classic
        # `address name [aliases...]` file, `#` comments. The resolver is the
        # client's and reads this before it asks the network, so `ping
        # localhost` resolves with nothing on the wire -- and `127.0.0.1` is
        # lwIP's own loopback, which answers it.
        ("file", "hosts",
         b"# Sys:S/hosts -- names the resolver knows without DNS (specs/net.md).\n"
         b"# address name [aliases...], one per line; # starts a comment.\n"
         b"127.0.0.1 localhost\n"),
        # What the machine's adapters should be at boot (specs/net.md), read by
        # the `netconfig` command from Startup-Sequence and applied through the
        # stack's control port -- the stack never reads it. `hostname` names the
        # machine and rides DHCP option 12; each adapter section is DHCP or the
        # static addresses, and an adapter with no section stays down. NE0 is the
        # stack's own name for the virtio link (eth.virtio0), and dhcp is what
        # QEMU's slirp answers -- the numbers are the network's, not the file's.
        ("file", "network.manifest",
         b"# Sys:S/network.manifest -- the adapters at boot (specs/net.md).\n"
         b"format = 1\n"
         b"hostname = aegir\n"
         b"\n"
         b"[NE0]\n"
         b"dhcp = true\n"),
        # Resource limits, opt-in (specs/limits.md): the shipped file is
        # comments plus the commented-out default example, so out of the box
        # it restricts nothing. An operator edits it without reimaging.
        ("file", "limits.manifest",
         b"# Sys:S/limits.manifest -- resource limits, opt-in (specs/limits.md).\n"
         b"#\n"
         b"# With no matching rule a user may use as much memory as the pool holds:\n"
         b"# the limit is the machine. A rule adds visibility (log) or a backstop\n"
         b"# (deny). A rule is <resource>_<action> = amount, in a subject's section.\n"
         b"# Subjects are the default, a class, or a user; a user's class comes from\n"
         b"# the user database (specs/auth.md), and a user: section overrides its class.\n"
         b"\n"
         b"[default]\n"
         b"# memory_log = 64M\n"),
        # Run once by the system boot session, then closed (specs/boot.md). A
        # sequence line is a command like any other's, so it starts a program --
        # the point of the boot session's launcher. The network comes up here,
        # not in a boot service (specs/net.md): `netconfig` reads
        # Sys:S/network.manifest (Sys: is up by construction when the boot
        # session runs this) and applies it through the stack's control port;
        # `ping` proves the path with an ICMP echo to the DHCP-supplied gateway
        # (no argument: the command asks the stack for the gateway, so nothing
        # names the machine); `ping localhost` proves the names path -- the
        # resolver reads Sys:S/hosts in the client, and 127.0.0.1 is lwIP's own
        # loopback, so nothing touches the wire. `tftp` fetches a file from the
        # virtual host's TFTP server over UDP -- the socket port's datagram
        # slice, exercised with real bytes on the wire -- and `tcpecho` carries a
        # string out and back over the loopback, the stream slice's whole
        # lifecycle with nothing on the wire at all (specs/net.md). `filenote`
        # is the marker no other acceptance step cues on, so the boot window
        # stays hidden. EndCLI >NIL: is the Amiga's quiet close; it needs the
        # NIL: handler. `net NE0/ipv4_address` reads the address back through
        # the Net: volume -- the live half, exercised with the ordinary file
        # protocol after DHCP has answered -- and the two hostname lines prove
        # the write half: an open, a write, a close, then a read of what the
        # stack now holds.
        ("file", "Startup-Sequence",
         b"; Aegir system startup: the network, then the quiet close.\n"
         b"netconfig\nping\nping localhost\ntftp aegir.txt\ntcpecho\ntcpbulk\n"
         b"net NE0/ipv4_address\nnet NE0/hostname\nnet NE0/hostname aegir-live\n"
         b"netsmoke\n"
         b"filenote Sys:VER.TXT aegir\nEndCLI >NIL:\n"),
        # The interpreter's acceptance (specs/shell.md): a built-in changes the
        # shell, and the next line only runs if the script did -- the shell
        # expands the alias x to date, so date starting is the proof.
        ("file", "Interpreter-Test",
         b"; the interpreter runs a built-in, then uses it.\nAlias x date\nx\n"),
        # Substitution (specs/shell.md): the command word and its argument are
        # variables the shell expands before it looks the command up, so the
        # command starting names the variable's value.
        ("file", "Subst-Test",
         b"; $prog and $value are the shell's environment.\n$prog $value\n"),
        # A .KEY script: {text} is the declared parameter and $1 the same
        # supplied argument, so the command starting proves both bound.
        ("file", "Params-Test",
         b".KEY text/A\n; {text} is the .KEY parameter, $1 the argument.\n"
         b"aegir-echo {text} $1\n"),
        # Control flow (specs/shell.md): If/Else/EndIf, Lab/Skip and the
        # condition words. A branch that must not run exits 9x, at or above the
        # fail level, so the file drops there instead of going on; a correct run
        # reaches only the last line, whose exit 42 is the acceptance's cue.
        ("file", "Control-Test",
         b"; the control words of a command file.\n"
         b"If EXISTS Sys:S/Control-Test\n"
         b"    ; the file exists, so the then-body runs\n"
         b"Else\n    aegir-echo 90\nEndIf\n"
         b"If EXISTS Sys:S/No-Such-File\n    aegir-echo 91\n"
         b"Else\n    ; a missing path is false, so the else-body runs\nEndIf\n"
         b"If NOT EXISTS Sys:S/No-Such-File\n"
         b"    ; NOT makes a missing path true\n"
         b"Else\n    aegir-echo 92\nEndIf\n"
         b"Set ctl 5\nIf $ctl GT 4\n"
         b"    ; a variable operand and a numeric comparison\n"
         b"Else\n    aegir-echo 93\nEndIf\n"
         b"If abc EQ ABC\n"
         b"    ; the comparison is case-blind\n"
         b"Else\n    aegir-echo 94\nEndIf\n"
         b"Skip tail\n    aegir-echo 95\nLab tail\n"
         b"aegir-echo 42\n"),
        # `NEWSHELL FROM <file>` (specs/launch.md): the new shell runs this
        # instead of Shell-Startup. Its exit code 77 is the acceptance's cue --
        # unique to the FROM run, so it cannot match an earlier command.
        ("file", "Nested-Startup",
         b"; the newshell FROM startup (specs/launch.md).\naegir-echo 77\n"),
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


# The faces `Sys:Fonts` ships (specs/fonts.md). The source is the vendored tree
# (specs/third_party.md); the destination is where a person finds it, because
# the catalog indexes a face by its *own* name and not by its path. Latin,
# Greek and Cyrillic, monospace, Arabic, Hebrew, and one CJK collection -- the
# CJK faces are large as a class and the volume carries one; a built subset is
# the escape hatch if its size ever bites.
ROOT = Path(__file__).resolve().parent.parent
FONT_SOURCES = [
    ("projects/terminus-font/ter-u12n.bdf", "Fonts/Terminus/ter-u12n.bdf"),
    ("projects/terminus-font/ter-u12b.bdf", "Fonts/Terminus/ter-u12b.bdf"),
    ("projects/noto-fonts/hinted/ttf/NotoSans/NotoSans-Regular.ttf",
     "Fonts/Noto/NotoSans-Regular.ttf"),
    ("projects/noto-fonts/hinted/ttf/NotoSansMono/NotoSansMono-Regular.ttf",
     "Fonts/Noto/NotoSansMono-Regular.ttf"),
    ("projects/noto-fonts/hinted/ttf/NotoSansArabic/NotoSansArabic-Regular.ttf",
     "Fonts/Noto/NotoSansArabic-Regular.ttf"),
    ("projects/noto-fonts/hinted/ttf/NotoSansHebrew/NotoSansHebrew-Regular.ttf",
     "Fonts/Noto/NotoSansHebrew-Regular.ttf"),
    ("projects/noto-fonts/archive/unhinted/NotoSansCJK/NotoSansCJK-Regular.ttc",
     "Fonts/Noto/NotoSansCJK-Regular.ttc"),
]


def check_font(path: str, data: bytes) -> None:
    """A font file's magic must match its extension (specs/fonts.md).

    The vendored `noto-fonts` tree holds an HTML page where a CJK collection
    should be, so a fetch can land something that is not a font at all. Packing
    it would embed the bytes and fail at the first *load*, far from the cause;
    this is where the bad path is still obvious."""
    suffix = path.rsplit(".", 1)[-1].lower()
    magics = {
        "bdf": (b"STARTFONT",),
        "pcf": (b"\x01fcp",),
        "ttf": (b"\x00\x01\x00\x00", b"true"),
        "otf": (b"OTTO",),
        "ttc": (b"ttcf",),
    }
    if suffix not in magics:
        raise SystemExit(f"make_disk: {path}: unknown font extension")
    if not data.startswith(magics[suffix]):
        raise SystemExit(
            f"make_disk: {path}: is not a {suffix} font -- its first bytes are "
            f"{data[:8]!r}, not one of {magics[suffix]!r} (a bad fetch?)")


def font_tree() -> list:
    """`Sys:Fonts`, read from the vendored faces."""
    root: dict = {}
    for source, destination in FONT_SOURCES:
        data = (ROOT / source).read_bytes()
        check_font(destination, data)
        # The destination starts `Fonts/`; `aegir_tree` makes that directory.
        parts = destination.split("/")[1:]
        node = root
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = data

    def to_tree(node: dict) -> list:
        entries = []
        for name, value in node.items():
            if isinstance(value, dict):
                entries.append(("dir", name, to_tree(value)))
            else:
                entries.append(("file", name, value))
        return entries

    return to_tree(root)


def aegir_tree(commands, datatypes, development=()) -> list:
    """The system volume's tree, with the command set as Sys:C and the datatype
    classes as Sys:DataTypes.

    `commands` is a list of (name, bytes): each becomes C/<name>, the flat
    lowercase command name the shell resolves (specs/dos.md). `datatypes` is the
    same for DataTypes/<name>, the classes a file is decoded by
    (specs/datatypes.md). `development` is the `Sys:Development` tree's children
    (specs/development.md) -- the compiler under C, the sysroot under Include and
    Libs -- and is empty for a target that does not carry it."""
    tree = list(AEGIR_BFS_TREE)
    # The datatypes acceptance's images (specs/datatypes.md): the demo opens
    # them through the ilbm, png and jpeg classes, on the session's namespace.
    tree.append(("file", "TestImage.ilbm", make_ilbm()))
    tree.append(("file", "TestImage.png", make_png()))
    tree.append(("file", "TestImage.jpg", make_jpeg()))
    # A PNG named `.ilbm` (specs/datatypes.md's content-first walk): the name
    # hints ilbm.datatype, which declines, and the broker walks `DataTypes:`
    # until png.datatype claims it by content. The demo opens it to prove the
    # class, not the name, decides.
    tree.append(("file", "Mystery.ilbm", make_png()))
    # Bigger than one volume envelope (936 bytes), so `copy`'s sendfile moves it
    # a frame at a time -- and into a `Home:` directory the union's write-frame
    # carries the filled frame to the member. The copy step is the acceptance's
    # proof of both; 6 KiB crosses several frames.
    tree.append(("file", "BIG.TXT", bytes(range(256)) * 24))
    png_class = next((data for name, data in datatypes if name == "png.datatype"), None)
    if commands:
        c_files = [("file", name, data) for name, data in commands]
        # The program's own class (specs/datatypes.md): a class shipped beside a
        # program's binary is resolved before `DataTypes:`. `png.datatype` sits
        # in `C:` with the demo, so the demo's class search finds it there and
        # the cue names `C:png.datatype` -- proof the program-directory half of
        # the search ran.
        if png_class is not None:
            c_files.append(("file", "png.datatype", png_class))
        tree.append(("dir", "C", c_files))
    if datatypes:
        dt_files = [("file", name, data) for name, data in datatypes]
        # The system's `aaa.datatype` (specs/datatypes.md's same-name override):
        # the user's `Home:DataTypes/aaa.datatype` is the png class and this is
        # the ilbm class, so the name is shared and the user's must shadow the
        # system's. The broker resolves `DataTypes:aaa.datatype` to the user's
        # (png), which claims a PNG; a system-first union would reach this ilbm
        # class instead, which declines one, and the cue would name `png.datatype`
        # -- so the acceptance's `open DataTypes:aaa.datatype` step fails if the
        # override does not hold.
        ilbm_class = next((data for name, data in datatypes if name == "ilbm.datatype"), None)
        if ilbm_class is not None:
            dt_files.append(("file", "aaa.datatype", ilbm_class))
        tree.append(("dir", "DataTypes", dt_files))
    tree.append(("dir", "Fonts", font_tree()))
    # The user's home (specs/auth.md's Homes, specs/session.md's User-Startup):
    # the row's `Sys:Homes/rroland`, made and owned at login. `S/User-Startup`
    # is what the session's own shell runs once, before Shell-Startup. And
    # `DataTypes/aaa.datatype` is the add-on proof (specs/datatypes.md): a class
    # a user drops into `Home:DataTypes` is found through the union, so the
    # broker's walk finds it and it claims -- a PNG under a name no extension
    # hints at, caught by the user's class before the system's (this one is the
    # png class under a name that sorts first, so the union's user member is the
    # one the broker reaches).
    home_children = [
        ("dir", "S", [
            ("file", "User-Startup",
             b"; the session's additions, run once (specs/session.md).\n"
             b"Echo UserStartup-OK\n"),
        ]),
    ]
    if png_class is not None:
        home_children.append(
            ("dir", "DataTypes", [("file", "aaa.datatype", png_class)]))
    tree.append(("dir", "Homes", [("dir", "rroland", home_children)]))
    if development:
        tree.append(("dir", "Development", list(development)))
        # A target that carries the compiler runs it once at session start, so
        # the acceptance cues on the compiler's own lines rather than an
        # interactive step (specs/development.md). The scale acceptance's huge
        # command runs after it, the same way (specs/memory.md).
        tree = _with_startup(tree, b"Sys:Development/C/cc\n"
                                   b"Sys:Development/C/aegir-big\n"
                                   b"Sys:Development/C/posix-test\n"
                                   b"Sys:Development/C/posix-path-test\n"
                                   b"Sys:Development/C/posix-file-test\n"
                                   b"Sys:Development/C/posix-memory-test\n"
                                   b"Sys:Development/C/posix-env-test\n")
    return tree


def _with_startup(tree: list, extra: bytes) -> list:
    """A copy of `tree` whose `S/Shell-Startup` runs `extra` after its own
    lines, without mutating the shared BFS tree."""
    result = []
    for node in tree:
        if node[0] == "dir" and node[1] == "S":
            children = []
            for child in node[2]:
                if child[0] == "file" and child[1] == "Shell-Startup":
                    children.append(("file", child[1], child[2] + extra))
                else:
                    children.append(child)
            result.append(("dir", node[1], children))
        else:
            result.append(node)
    return result


def development_tree(root) -> list:
    """The `Sys:Development` tree's children (specs/development.md): `root` laid
    out as Development/<name>..., the compiler under C and the sysroot under
    Include and Libs. An absent `root` is no development tree."""
    if root is None or not root.is_dir():
        return []

    def walk(directory: Path) -> list:
        children = []
        for path in sorted(directory.iterdir()):
            if path.is_dir():
                children.append(("dir", path.name, walk(path)))
            elif path.is_file():
                children.append(("file", path.name, path.read_bytes()))
        return children

    return walk(root)


def partition_table(commands, datatypes, development=()) -> list:
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
            # The system volume holds the sessions' data -- Home:, ENV:, S: --
            # as well as the boot tree, and a filesystem with no free blocks
            # cannot take a session's first write. `bfs_minimum_bytes` is the
            # tree alone, so the reserve is a second tree's worth: the volume
            # is sized to be written, not only read (specs/bfs.md).
            needed = bfs_minimum_bytes(aegir_tree(commands, datatypes, development), name) * 2
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
    parser.add_argument(
        "--datatypes", type=Path, default=None,
        help="a directory of datatype class images, one per file, packed as "
             "Sys:DataTypes (specs/datatypes.md)")
    parser.add_argument(
        "--development", type=Path, default=None,
        help="a directory laid out as the Sys:Development tree -- the compiler "
             "under C and the sysroot under Include and Libs "
             "(specs/development.md)")
    args = parser.parse_args()

    commands = []
    if args.commands is not None and args.commands.is_dir():
        commands = [(path.name, path.read_bytes())
                    for path in sorted(args.commands.iterdir()) if path.is_file()]
    datatypes = []
    if args.datatypes is not None and args.datatypes.is_dir():
        datatypes = [(path.name, path.read_bytes())
                     for path in sorted(args.datatypes.iterdir()) if path.is_file()]
    development = development_tree(args.development)
    partitions = partition_table(commands, datatypes, development)
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
        for name, tree in (("AEGIR", aegir_tree(commands, datatypes, development)),
                           ("BFS", BFS_TREE)):
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
