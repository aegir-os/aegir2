# The FAT formats

FAT is Aegir's interchange filesystem: the format a disk written elsewhere is
read from and written to, not the format Aegir keeps its own things in. This
spec fixes what `aegir-fs-fat` speaks, how it decides which format it is
looking at, and the long-name grammar the volume protocol's paths are matched
against (`specs/vfs.md`). The transport — the block port and the range grant —
is `specs/services.md`; the protocol is `specs/vfs.md`.

## What is spoken, and what is refused

| Format | Boot sector | Spoken |
| ------ | ----------- | ------ |
| FAT16  | BPB, fixed root region | yes |
| FAT32  | BPB, root is a cluster chain | yes |
| FAT12  | BPB, cluster count under 4085 | **refused by name** |
| ExFAT  | `EXFAT   ` at offset 3 | **refused by name** |

A recognized format the service does not speak is a **loud refusal**, not a
best-effort parse: a FAT12 chain read as FAT16, or ExFAT's boot sector read as
a BPB, would be silent corruption — the exact failure a filesystem must not
have. The service prints which format it found and halts.

## Which FAT is here

The FAT type is decided by the **cluster count**, the format's own rule
(Microsoft's FAT specification, "Determination of FAT Type"): under 4085
clusters is FAT12, under 65525 is FAT16, and the rest is FAT32. The count is
`(total sectors − data start) / sectors per cluster`, both from the BPB.

One field must be allowed to overrule the count, and it is the root's shape:
the **root entry count** at offset 17 is zero exactly when the root is a
cluster chain, and a chain root is what the walk follows. A small volume whose
root is a chain (a tool asked for FAT32 on a partition the count would call
FAT16) is still FAT32's layout, so `root_entries == 0` decides FAT32 and the
count decides only between FAT12 and FAT16 on a root that is a fixed region.
This is the one place the flavor is chosen; every walk after it branches on
`fat32`.

## Names

A directory holds 32-byte slots. A short (8.3) name is eleven bytes — base of
eight, extension of three, space-padded, uppercase. A **long name** lives in a
run of slots that immediately precede the 8.3 slot it belongs to, connected to
it by a checksum so a stale run cannot be adopted by a neighbour.

### The long-name slot (VFAT)

Each long-name slot is 32 bytes, little-endian throughout:

| Offset | Bytes | Holds |
| ------ | ----- | ----- |
| 0      | 1     | sequence number; `0x40` set on the first physical slot, `0x80` never set on a live one |
| 1–10   | 10    | five UTF-16LE code units |
| 11     | 1     | attribute `0x0f` — what marks the slot as a long-name fragment |
| 12     | 1     | type, zero |
| 13     | 1     | the short name's checksum |
| 14–25  | 12    | six UTF-16LE code units |
| 26–27  | 2     | first cluster, zero |
| 28–31  | 4     | two UTF-16LE code units |

So thirteen code units per slot. The run is stored in reverse: reading forward
from disk, the highest sequence number (with `0x40`) comes first and sequence
number one comes last, immediately before the 8.3 slot. A name is terminated
by `0x0000` and padded to the slot's end with `0xFFFF`; the format's ceiling is
255 code units.

The checksum is over the eleven bytes of the 8.3 name, one byte at a time:

```
sum = 0
for byte in short_name[0..10]:
    sum = (((sum & 1) << 7) + (sum >> 1) + byte) & 0xff
```

Every slot of a run carries the same checksum, and the 8.3 slot's own name must
produce it; a run whose checksum does not match is not this entry's.

### Matching

A path component is matched **long name first, then short name**, both
ASCII-case-folded (`a`–`z` folded to `A`–`Z`, everything else compared as
bytes): FAT is case-insensitive, and a caller who writes `readme.txt` means the
entry whose name is `README.TXT`. The long name is carried as **UTF-8** — the
UTF-16 units decoded to UTF-8 and passed through — and non-ASCII bytes are
compared byte-for-byte. There is no Unicode case folding; the ASCII fold is
the whole rule.

### Making a short name

A long name that is not already 8.3-clean gets a generated alias so the entry
can exist at all: up to six characters of the long base with illegal
characters replaced by `_` and letters uppercased, then `~`, then a decimal
counter starting at one, and the extension from the last dot (its first three
legal characters). A counter is bumped until the alias is free in that
directory. An 8.3-clean long name needs no alias: the entry's short name is
the uppercased name itself, and the NT case flags in byte 12 of the 8.3 slot
(bit `0x08` base, bit `0x10` extension) record where the original was
lowercase, so the name that comes back is the name that went in.

The long-name charset excludes `" * / : < > ? \ |` and the control bytes
`0x00`–`0x1f`, and never ends in a space or a dot. A short name is legal
letters, digits, `-` and `_` only. A name past those is refused, not mangled.

## Times

A 8.3 slot carries creation, last-write and last-access date-time words in
DOS format: date as `(year−1980)<<9 | month<<5 | day`, time as
`hour<<11 | minute<<5 | second/2`. There is no time source in the system
yet, so the service currently writes zeroes and reports none; the fields are
read and written for real once the clock source lands (the arc's tail), and
the service gets the time from the clock port rather than inventing one.

## Deferred

Written down so the omissions are decisions, not surprises:

- **No space query.** `statvfs` and the runtime's `statfs` are not answered;
  the free-cluster count is not cached (the allocation scan is linear, and
  that cost is known and accepted for now).
- **No FAT12 or ExFAT support.** They are refused, above.
- **Attribute bits** other than directory are not honored; there is no
  `chmod`.
- **No bad-cluster or loop detection** in a chain walk.
- The FAT32 FSInfo free count is marked unknown rather than maintained, and
  the backup copy at sector 7 is not synced.
