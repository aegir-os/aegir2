# The Be File System (BFS)

BFS is Aegir's **primary filesystem**: the format the system volume will
stand on, and the one that carries the metadata the Workbench and the rest of
the desktop read — icons, file types, tooltypes, and whatever else names a
file's meaning. FAT stays the interchange filesystem (`specs/fat.md`); BFS is
where Aegir keeps its own things.

This spec fixes the on-disk format, the metadata and query model, and the
extensions Aegir adds. The transport — the block port and the range grant — is
`specs/services.md`; the volume protocol's base methods are `specs/vfs.md`.

## Reference

The format is the Be File System, as implemented by Haiku. The implementation
is **greenfield** — no Haiku code is copied or built — but it is written
against Haiku's as the established implementation, and it produces and reads
the same bytes. The revision consulted is Haiku `89436a13` (2026), whose BFS
add-on lives at `src/add-ons/kernel/file_systems/bfs/`:

| File | What it fixes |
| ---- | ------------- |
| `bfs.h` | the on-disk structures: superblock, inode, block run, data stream, small data |
| `Volume.cpp` | superblock at offset 512, validity, initialization, block addressing |
| `Inode.cpp` | data streams, small data, attribute stream, tree keys |
| `BPlusTree.{h,cpp}` | the B+tree header and node |
| `Index.{h,cpp}` | the index directory and the standard indices |
| `Attribute.{h,cpp}` | the attribute model |
| `Query.{h,cpp}` | the query language and live queries |
| `Journal.{h,cpp}` | the log, replay and commit |
| `BlockAllocator.{h,cpp}` | allocation groups, bitmaps, trim |
| `src/bin/mkfs/` | how a volume is created |

Where this spec gives a number, the number is Haiku's; where it gives a
behaviour, the behaviour is Haiku's. Aegir's additions are named as such and
are gated so Haiku is unaffected.

## Compatibility and the extension gate

Aegir's BFS is **byte-compatible**: a volume we create mounts on Haiku and
vice versa. Additions beyond what BeOS and Haiku understand are gated behind a
flag word that Haiku ignores: the superblock's reserved area, `_reserved[0]`,
is `AEGIR_FLAGS`, a little-endian `uint32`.

| Bit | Name | Meaning |
| --- | ---- | ------- |
| 0 | `AEGIR_BFS_SPARSE` | a file on this volume may have a hole (see Sparseness) |
| 1 | `AEGIR_BFS_JOURNAL_FULL` | the volume's metadata *and* data are journaled (see Journal) |

A reader that does not know `AEGIR_FLAGS` reads the superblock as it always
has. An extension that changes what a file *reads as* is only usable when the
reader knows the flag; the flag is how a mount decides whether it may serve
the volume. The one such extension today is sparseness, and it is the reason
the gate exists.

## Byte order and the superblock

BFS is **little-endian**; the superblock's `fs_byte_order` is `'BIGE'` (`'BIGE'`
read as the bytes of an int32), which identifies a little-endian BFS. Every
integer in this spec is little-endian on disk.

The volume's first 512 bytes are a boot block (erased by `mkfs`, "so a
leftover boot sector cannot confuse identification"). The **superblock is the
512 bytes at offset 512** — the `disk_super_block` struct is exactly 512 bytes
— and the first BFS block (block 0) is those 1024 bytes rounded up to the
block size. The superblock is read by `Volume::Identify`, which reads 1024
bytes and checks offset 512 first (offset 0 is the PowerPC-era location, which
little-endian BFS does not use).

| Offset | Size | Field | Notes |
| ------ | ---- | ----- | ----- |
| 0 | 32 | `name` | volume name, NUL-padded; no `/` |
| 32 | 4 | `magic1` | `'BFS1'` |
| 36 | 4 | `fs_byte_order` | `'BIGE'` |
| 40 | 4 | `block_size` | 1024, 2048, 4096 or 8192 |
| 44 | 4 | `block_shift` | log2 of `block_size` |
| 48 | 8 | `num_blocks` | the volume's size in blocks |
| 56 | 8 | `used_blocks` | allocated blocks |
| 64 | 4 | `inode_size` | **equals `block_size`**: one inode is one block |
| 68 | 4 | `magic2` | `0xdd121031` |
| 72 | 4 | `blocks_per_ag` | **bitmap blocks per allocation group** |
| 76 | 4 | `ag_shift` | log2 of blocks per allocation group |
| 80 | 4 | `num_ags` | number of allocation groups |
| 84 | 4 | `flags` | `'CLEN'` or `'DIRT'` — the clean/dirty state |
| 88 | 8 | `log_blocks` | the log's `block_run` |
| 96 | 8 | `log_start` | log replay pointer (a block number, see Journal) |
| 104 | 8 | `log_end` | log commit pointer |
| 112 | 4 | `magic3` | `0x15b6830e` |
| 116 | 8 | `root_dir` | the root directory's `inode_addr` |
| 124 | 8 | `indices` | the indices directory's `inode_addr` |
| 132 | 32 | `_reserved[8]` | **`_reserved[0]` is `AEGIR_FLAGS`**; the rest zero |
| 164 | 348 | `pad_to_block[87]` | unused |

Superblock validity (Haiku's `IsValid`): the three magics, byte order,
`block_size == inode_size`, `1 << block_shift == block_size`, `num_ags >= 1`,
`ag_shift >= 1`, `blocks_per_ag >= 1`, `num_blocks >= 10`, and
`num_ags == ceil(num_blocks / (1 << ag_shift))`.

## Blocks, allocation groups and addressing

A **block run** is the one unit of allocation, eight bytes packed:

```
struct block_run { int32 allocation_group; uint16 start; uint16 length; }
```

`start` and `length` are within the allocation group; `length` is capped at
**65535** (`MAX_BLOCK_RUN_LENGTH` — the field is 16 bits and zero is not used
as "65536"). An `inode_addr` *is* a `block_run`. A zero run (`IsZero`) is the
end of a run list.

A block number is global; a run maps to one:

```
ToBlock(run) = (run.allocation_group << ag_shift) | run.start
ToBlockRun(block) = { block >> ag_shift, block & ((1 << ag_shift) - 1), 1 }
```

The **block bitmap** for allocation group `i` occupies `blocks_per_ag` blocks
starting at block `1 + i * blocks_per_ag`; block 0 is the boot block and
superblock. The **log** starts at block `num_bitmap_blocks + 1` and is
`log_size` blocks long (512, 2048 or 4096; `mkfs` picks by size). The rest of
the volume is data. A fresh volume reserves the bitmap and the log in the
bitmap, and `used_blocks` starts at their total.

`mkfs` sizes the allocation groups from the geometry: `group_shift` starts at
13 and rises (capped at 16, with `blocks_per_ag` doubling) until the group
count is at most **56** (`kDesiredAllocationGroups`). `blocks_per_ag` is the
number of bitmap blocks per group, not a data-block count.

## Inodes

One inode is one block. An inode in use begins with magic `0x3bbe0ad9`
(`INODE_MAGIC1`).

| Offset | Size | Field | Notes |
| ------ | ---- | ----- | ----- |
| 0 | 4 | `magic1` | `0x3bbe0ad9` |
| 4 | 8 | `inode_num` | this inode's `block_run` |
| 12 | 4 | `uid` | |
| 16 | 4 | `gid` | |
| 20 | 4 | `mode` | POSIX mode plus BFS's extended type bits |
| 24 | 4 | `flags` | see below |
| 28 | 8 | `create_time` | |
| 36 | 8 | `last_modified_time` | |
| 44 | 8 | `parent` | the parent directory's `block_run` |
| 52 | 8 | `attributes` | the attribute directory's `block_run` (zero when none) |
| 60 | 4 | `type` | an attribute's `type_code`, else zero |
| 64 | 4 | `inode_size` | `block_size` |
| 68 | 4 | `etc` | |
| 72 | 144 | `data` / `short_symlink` | a `data_stream`, or a symlink target of at most 144 bytes including NUL |
| 216 | 8 | `status_change_time` | |
| 224 | 8 | `pad[2]` | |
| 232 | … | `small_data_start` | inline attributes, to the end of the block |

`flags`:

| Bit | Name | Meaning |
| --- | ---- | ------- |
| 0x0001 | `INODE_IN_USE` | always set on a live inode |
| 0x0004 | `INODE_ATTR_INODE` | this inode is an attribute |
| 0x0008 | `INODE_LOGGED` | data-stream changes are logged |
| 0x0010 | `INODE_DELETED` | pending deletion |
| 0x0040 | `INODE_LONG_SYMLINK` | the symlink target is in the data stream |
| 0xffff | `INODE_PERMANENT_FLAGS` | the bits that survive a write-back |

`mode`'s low bits are POSIX (`S_IFMT`, permissions); BFS adds extended types
that tell a container, an attribute directory, an attribute, and an index
apart:

| Value | Name | Meaning |
| ----- | ---- | ------- |
| `01000000000` | `S_ATTR_DIR` | an attribute directory |
| `02000000000` | `S_ATTR` | an attribute |
| `04000000000` | `S_INDEX_DIR` | an index, or the index directory |
| `00100000000` … | `S_STR_INDEX` … | the index's key type (string, int32, uint32, int64, uint64, float, double) |
| `00002000000` | `S_ALLOW_DUPS` | duplicate keys allowed |

### Data streams

A file's `data` names its blocks as an **extent tree**:

```
struct data_stream {
    block_run direct[12];
    int64 max_direct_range;
    block_run indirect;
    int64 max_indirect_range;
    block_run double_indirect;
    int64 max_double_indirect_range;
    int64 size;
}
```

The twelve direct runs cover the first `max_direct_range` bytes. Past that,
the `indirect` block run names a block full of runs (a run array), covering to
`max_indirect_range`. Past that, `double_indirect` names a block of run-array
blocks covering to `max_double_indirect_range`. `size` is the file's logical
size. A run array block holds `block_size / 8` runs. The `max_*_range` fields
are the offsets each level reaches; they are what a seek binary-searches.
`NUM_ARRAY_BLOCKS` (4) and `DOUBLE_INDIRECT_ARRAY_SIZE` (4096) are the format
constants for the double-indirect fan-out.

Crucially, a run walk **stops at the first zero run**. A non-sparse file that
uses two of its twelve direct runs has zeroes after the second, and that is
the terminator. A file with a hole in the middle therefore cannot be read by
Haiku past the hole — which is why sparseness is gated (Sparseness).

## Attributes

An attribute is a typed, named value attached to an inode. It is the whole
point of BFS here: icons, file types and tooltypes are attributes.

**Small data** is an attribute kept in the inode itself, in the bytes after
the fixed part. Each is:

```
struct small_data { uint32 type; uint16 name_size; uint16 data_size;
                    char name[]; /* data at Name() + NameSize() + 3 */ }
```

The name is NUL-terminated and padded to a four-byte boundary, which is the
`+ 3` and the `+ 1` in `Size()`. The entries are walked until the next one
would fall outside the inode. Every inode carries one reserved entry: the
**file name**, `type = 'CSTR'`, with the one-byte name `0x13` (`FILE_NAME_NAME`)
and the name as its data. A regular attribute uses its own name and its
`type_code` as `type`.

**An attribute too big for the inode** — or created when the inode has no room
— gets an inode of its own. The first such attribute creates the inode's
**attribute directory**: an inode whose mode is `S_ATTR_DIR`, referenced from
the inode's `attributes` block run. Its data stream is a B+tree mapping the
attribute's name to the attribute inode's block number; the attribute inode
has mode `S_ATTR`, flag `INODE_ATTR_INODE`, and its `type` field is the
attribute's `type_code`. An attribute's value is the attribute inode's data
stream, so it is read and written exactly like a file.

A volume's attributes are therefore in one of two places, and every operation
looks in the inode's small data first and the attribute directory second.
Attribute names are at most 255 bytes; a name that would not fit is refused,
not truncated.

## Directories and the B+tree

A directory is an inode whose data stream is a **B+tree** mapping a name to
the block number of the inode it names. The tree's header and nodes live in
the data stream like any other data, which is what makes a directory grow past
one block without any special case.

The B+tree header (`bplustree_header`) is at the start of the stream:

| Field | Notes |
| ----- | ----- |
| `magic` | `0x69f6c2e8` |
| `node_size` | 1024 by default; a power of two, at least the block size |
| `max_number_of_levels` | a bound, for validation |
| `data_type` | the key type (below) |
| `root_node_pointer` | the root node's block, or `-1` when empty |
| `free_node_pointer` | the free list, or `-1` |
| `maximum_size` | the stream's size bound |

A node (`bplustree_node`) is:

| Field | Notes |
| ----- | ----- |
| `left_link` / `right_link` | sibling links, or `BPLUSTREE_NULL` (`-1`) |
| `overflow_link` | `-1` on a leaf; a duplicate array or child link otherwise |
| `all_key_count` | number of keys |
| `all_key_length` | total bytes of keys |

After the fixed part come the keys, then an array of `uint16` key lengths
(rounded up to an `off_t` boundary), then an array of `off_t` values. A node
is a leaf exactly when `overflow_link == BPLUSTREE_NULL`; an internal node's
values are child node blocks. `BPLUSTREE_FREE` (`-2`) marks a free node.

Keys are compared by type:

| `data_type` | Name | Key |
| ----------- | ---- | --- |
| 0 | `BPLUSTREE_STRING_TYPE` | bytes, compared as a string |
| 1 | `BPLUSTREE_INT32_TYPE` | int32 |
| 2 | `BPLUSTREE_UINT32_TYPE` | uint32 |
| 3 | `BPLUSTREE_INT64_TYPE` | int64 |
| 4 | `BPLUSTREE_UINT64_TYPE` | uint64 |
| 5 | `BPLUSTREE_FLOAT_TYPE` | float |
| 6 | `BPLUSTREE_DOUBLE_TYPE` | double |

The key type is derived from the stream's mode (`ModeToKeyType`) or from a
`type_code` (`TypeCodeToKeyType`). A tree may allow duplicate keys; those are
held in a **duplicate array** reached through `overflow_link`, with fragment
nodes past that (`NUM_DUPLICATE_VALUES` 125, `NUM_FRAGMENT_VALUES` 7). The name
index allows duplicates, because two directories may hold the same name.

A directory's keys are the entry names (UTF-8, at most 255 bytes) and its
values are inode block numbers. The root directory's block is the superblock's
`root_dir`; the index directory's is `indices`.

## Indices and comments on the standard set

The **index directory** is an inode with mode `S_INDEX_DIR | S_<type>_INDEX |
S_DIRECTORY`, named by the superblock's `indices`. Its data stream is a B+tree
mapping an index name to its inode. An index inode's data stream is a B+tree
mapping a *value* to the block numbers of the inodes that hold it. Four
indices are created at `mkfs`:

| Name | Key type |
| ---- | -------- |
| `name` | string (the entry name) |
| `BEOS:APP_SIG` | string (the preferred application's signature) |
| `last_modified` | int64 (the inode's modified time) |
| `size` | int64 (the file's size) |

Other attributes gain an index when one is created for them. An index makes a
query a tree walk instead of a volume scan; a query on an unindexed attribute
falls back to scanning.

## Times

An inode time is a 64-bit value: the seconds in the high 48 bits
(`time >> 16`) and a 16-bit sub-second field in the low bits. Haiku's encoding
(`bfs_inode::ToInode`) is

```
ToInode(usecs) = ((usecs / 1000000) << 16) | unique_from_nsec((usecs % 1000000) * 1000)
```

where `unique_from_nsec` spreads a value with the top nibble `0xf000` marking
"no sub-second part". Because BeOS read only `time >> 16`, a time written with
a sub-second part still reads as the right second on BeOS — the extension is
compatible. Aegir adopts Haiku's encoding. When the only clock is the RTC's
seconds (`specs/services.md`), the sub-second field is zero; a finer clock
fills it without any format change.

## The journal

BFS is a journaling filesystem. The log is the `log_blocks` run at the front
of the volume; `log_start` and `log_end` are block numbers that bound the
pending entries, and the superblock's `flags` is `'DIRT'` while entries are
pending and `'CLEN'` when the volume is clean.

A transaction writes, in order:

1. One or more **run array** blocks. A `run_array` is a block-sized header —
   `int32 count; int32 max_runs; block_run runs[]` — listing, in ascending
   block order, the target blocks the transaction changed. Its size is the
   block size, so a freshly allocated one is ready to use; its capacity is
   `(block_size - sizeof(run_array)) / sizeof(block_run)` minus one, for an
   off-by-one in Be's implementation.
2. Immediately after each array block, the **data blocks** for its runs, in
   order. So the log is a flat stream of `[array][data…][array][data…]`.

Commit is the superblock write that sets `flags = 'DIRT'` and `log_end` to the
position past the entry, then flushes the device. On mount, **replay** reads
entries from `log_start` to `log_end` and copies each run's data to its target
run; then it sets `log_start = log_end` and the volume clean. A clean volume
has `flags == 'CLEN'` and `log_start == log_end`, so replay is a no-op — the
**clean-volume fast mount**: no scanning, no replay.

Aegir's Phase 5 implements this log. Phase 3 (writes) runs without it: it
keeps `log_start == log_end` and the volume `'CLEN'` after every operation, so
each operation is atomic enough for a cleanly-managed volume and Haiku sees a
clean volume it can mount. `AEGIR_BFS_JOURNAL_FULL` marks a volume whose data
blocks, not only its metadata, are journaled; until that bit is set, the log
records metadata only (the same as BeOS's own choice).

## The service and the library

The implementation is split the way the repository is (`AGENTS.md`), and no
file is a monolith:

- `libs/freestanding/aegir-bfs/` — the format and its algorithms, usable
  without a libc: `layout.h` (the structs and byte order), `volume.{h,cc}`
  (superblock, block runs, block I/O through the client window),
  `inode.{h,cc}`, `bplustree.{h,cc}`, `allocator.{h,cc}`,
  `attribute.{h,cc}`, `index.{h,cc}`, `query.{h,cc}`, `journal.{h,cc}`.
- `apps/freestanding/aegir-fs-bfs/` — the volume service: bootstrap, announce,
  and the serve loop, in `aegir-fs-fat`'s shape.
- `libs/freestanding/aegir-metadata/` — the metadata and query protocol shared
  by the service, the VFS client and the runtime.
- `scripts/mkfs_bfs.py` — the host-side builder for the test disk.

The Aegir additions are **runtime features** wherever possible, because the
format need not change for them: TRIM is a discard on a freed run, permission
enforcement is a check against Aegir's accounts, and the clean fast mount is a
decision not to replay. Only sparseness changes what a read returns, so only
sparseness takes a gate bit.

## Metadata over the volume protocol

The volume protocol (`specs/vfs.md`) grows methods for attributes and queries.
The base methods answer refusals with an empty reply; metadata needs to tell
"this filesystem has no such attribute" from "this filesystem has no
metadata", so the new methods answer a **status word first**:

| Value | Name | Meaning |
| ----- | ---- | ------- |
| 0 | `kOk` | the call succeeded |
| 1 | `kUnsupported` | the filesystem does not implement this; the runtime maps it to `EOPNOTSUPP` |
| 2 | `kNotFound` | no such path, or no such attribute |
| 3 | `kInvalidName` | the name is empty or longer than 255 bytes |
| 4 | `kReadOnly` | the volume is read-only |
| 5 | `kNoSpace` | the volume is full |
| 6 | `kNotADirectory` / `kIsADirectory` | the kind is wrong for the call |

FAT answers **`kUnsupported`** to every metadata method and an empty list to
the list method; it never pretends. The runtime maps `kUnsupported` to
`EOPNOTSUPP`, `kNotFound` to `ENODATA`, and so on.

The methods mirror Haiku's `fs_*attr` API:

- **attr stat** — in: path words, name. Answer: status, `type_code`, size.
- **attr read** — in: path words, name, offset, max. Answer: status, bytes.
- **attr write** — in: path words, name, `type_code`, offset, bytes. Answer:
  status, written. Creating an attribute sets its type; writing it with a
  different type is a refusal.
- **attr remove** — in: path words, name. Answer: status.
- **attr list** — in: path words, index. Answer: status, name, `type_code`,
  size, or `kNotFound` past the last attribute. The cursor is the caller's
  index, the `list`/describe pattern.

Paths are the component path from `specs/vfs.md`, resolved against the root
inode through the directory trees. A path that names a file whose inode has no
attribute directory is `kNotFound`, not `kUnsupported` — the filesystem *has*
metadata, this file simply has none.

### Type codes and the attribute namespace

The `type_code` is Haiku's; values a client may set:

| Code | Name |
| ---- | ---- |
| `'CSTR'` | string |
| `'LONG'` / `'ULNG'` | int32 / uint32 |
| `'LLNG'` / `'ULLG'` | int64 / uint64 |
| `'SHRT'` / `'USHT'` | int16 / uint16 |
| `'BYTE'` / `'UBYT'` | int8 / uint8 |
| `'FLOT'` / `'DBLE'` | float / double |
| `'BOOL'` | boolean |
| `'RAWT'` | raw bytes |
| `'MIMS'` | MIME string (treated as a string) |

Names follow BeOS: a global attribute is `BEOS:<name>` (the shortest, and what
an icon and a type are), and anything else is `<mime-type>:<name>`. Aegir's
own additions use an `AEGIR:` prefix and are named in `aegir/metadata`:

| Attribute | Type | Meaning |
| --------- | ---- | ------- |
| `BEOS:TYPE` | `'MIMS'` | the file's MIME type — the Workbench's icon choice and the launcher's decision |
| `BEOS:APP_SIG` | `'CSTR'` | the preferred application's signature (an indexed attribute) |
| `BEOS:ICON` | `'RAWT'` | the file's icon, in the toolkit's format |
| `BEOS:MINI_ICON` | `'RAWT'` | the small icon |
| `AEGIR:TOOLTYPES` | `'CSTR'` | the Amiga tooltype list: newline-separated `KEY=VALUE` lines, an `.info` file flattened into one attribute |

Tooltypes are one attribute rather than many because the Amiga model is one
ordered list, and because a per-file attribute list with a fixed order is not
what small data is good at. The Workbench parses the lines.

## Queries

A query is Haiku's query language: an expression over attributes, e.g.

```
BEOS:TYPE == "text/plain" & size > 1024
```

with the operators `=`, `==`, `!=`, `<`, `<=`, `>`, `>=`, combined with `&`
(and) and `|` (or). `name`, `size` and `last_modified` are the standard
indexed attributes. A query uses an index when one exists for a term and
scans otherwise.

- **query open** — in: the query string, flags. Answer: status, a handle.
- **query next** — in: the handle. Answer: status, one entry (name, kind,
  size), or an end marker. The cursor is the filesystem's, per handle.
- **query close** — in: the handle. Answer: status.

A **live query** is a query that sees changes after it opens. Its open carries
a **notification endpoint** the filesystem may signal, plus a token:

- **query open live** — in: the query string, flags, token; one cap: the
  subscriber's notification endpoint. Answer: status, handle.
- When a change (create, remove, rename, attribute write) makes an inode enter
  or leave a live query's set, the filesystem **signals** the subscriber's
  endpoint with the handle. The signal is one-way (`seL4_Send`), so the
  filesystem's serve loop never blocks on a subscriber and a subscriber
  blocked in a write cannot deadlock it. The subscriber re-reads with query
  next, which reports changes since its last read.

The signal carries no data beyond the handle: what changed is discovered by
re-reading. This keeps the notification path off the critical section and is
the shape Phase 6 refines.

## Sparseness

A sparse file has holes: ranges of zeros with no blocks behind them. BFS's
extent model has no hole marker — a zero `block_run` ends the run list — so
Aegir represents a hole as a **zero run in the middle of the data stream** and
sets the inode's logical size and `max_*_range` fields across it. The inode
also carries `AEGIR_BFS_SPARSE` at the volume level.

This is the one extension Haiku cannot read past: its run walk stops at the
first zero run and every offset beyond the hole fails. It is therefore gated.
A volume with no sparse file never sets the bit and is fully Haiku-readable. A
volume that has one still mounts on Haiku, but a sparse file reads only to its
first hole. This is a decision, not a surprise. Phase 3 implements it; until
then the bit is never set.

## Extensions and their gates

| Extension | On-disk change | Gate |
| --------- | -------------- | ---- |
| TRIM on free | none | a device that answers `discard` (below) |
| Sub-second times | none (Haiku's own encoding) | a clock finer than a second |
| Clean no-replay fast mount | none | `flags == 'CLEN'` and `log_start == log_end` |
| Permission enforcement | none (uid/gid/mode are already stored) | Aegir accounts (`specs/auth.md`) |
| Sparseness | zero runs | `AEGIR_BFS_SPARSE` |
| Full data journaling | none | `AEGIR_BFS_JOURNAL_FULL` |

### TRIM

The block protocol (`specs/services.md`) grows a **capabilities** answer and a
**discard** method:

- **caps** — the answer, written into the caller's window, is a `BlockCaps`
  struct with a flags word; bit 0 is "discard is supported", plus the largest
  discard the device accepts. `Identify` keeps its fixed 24-byte wire and is
  not extended; a filesystem asks caps when it mounts.
- **discard** — words: first sector, sector count; clamped by the caller's
  badge exactly as read and write are. The driver translates it to the
  device's unmapped-range command. A device that does not support discard
  answers zero, and the filesystem stops asking.

The virtio-blk driver negotiates `VIRTIO_BLK_F_DISCARD` (feature bit 13) and
issues `VIRTIO_BLK_T_DISCARD` requests; the QEMU drive is given
`discard=unmap` in `scripts/targets.py`, since without it the backend does not
advertise the feature. BFS discards a run when it frees it, in
`BlockAllocator::Free`'s shape, coalescing adjacent frees so a directory's
death is a few large discards rather than many small ones.

## Deferred

Written down so the omissions are decisions:

- **Resize.** No grow or shrink; `ResizeVisitor` is not implemented.
- **fsck and repair.** Beyond journal replay, there is no checker; a corrupt
  volume is mounted read-only or refused.
- **ACLs.** Mode, uid and gid, not POSIX ACLs.
- **An on-system `mkfs`.** Volumes are built host-side by `scripts/mkfs_bfs.py`
  in this arc; a formatting service comes later.
- **Preallocation and defragmentation.**
- **Per-volume concurrency.** The service serves one call at a time, as the
  volume protocol defines.

## Decisions taken for review

These resolve the open points from the arc's plan; they are the parts most
worth a second look before code exists.

1. **`kUnsupported` is a status the filesystem reports, not a flag the VFS
   withholds.** The new metadata methods lead with a status word, so "no
   attribute" and "no metadata support" are different answers and FAT can be
   honest without the runtime knowing which volume is which.
2. **The extension gate is `_reserved[0]`, not the superblock `flags` field.**
   `flags` is BFS's clean/dirty state; `_reserved` is ignored by every reader,
   which is exactly what a gate needs.
3. **Sparseness is accepted as Haiku-unreadable past a hole**, gated by
   `AEGIR_BFS_SPARSE`. If that trade is unwanted, sparseness should be dropped
   rather than done another way, because no other encoding is compatible.
4. **The block capabilities are a new method**, not a wider `Identify`, so the
   partition manager's fixed wire and every existing caller are untouched.
5. **Live queries signal a one-way endpoint and re-read; they do not carry the
   change.** This keeps the single-threaded serve loop and makes deadlock
   impossible; the cost is a re-read per notification.
6. **The filesystem registry is data**, one row per GPT type GUID mapping to a
   kind prefix and an initrd binary, and the device manager hands the
   partition manager a named bundle of the helper images rather than one. This
   is `specs/services.md`'s business and lands with Phase 1.
