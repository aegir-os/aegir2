# The Be File System (BFS)

BFS is Aegir's **primary filesystem**: the format the system volume stands
on, and the one that carries the metadata the Workbench and the rest of
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
| 0 | `AEGIR_BFS_SPARSE` | unused: tail sparseness needs no gate (see Sparseness) |
| 1 | `AEGIR_BFS_JOURNAL_FULL` | the volume's metadata *and* data are journaled (see Journal) |

A reader that does not know `AEGIR_FLAGS` reads the superblock as it always
has. An extension that changes what a file *reads as* is only usable when the
reader knows the flag; the flag is how a mount decides whether it may serve
the volume. Sparse tails turned out to need no flag -- they are simply a size
past the runs -- so no bit is set today.

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
Haiku past the hole, which is why Aegir's sparseness is only the tail and
never a middle hole (Sparseness).

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
| `node_size` | `BPLUSTREE_NODE_SIZE`, hard-coded 1024 whatever the block size; a directory's stream is two nodes |
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

After the fixed part come the keys, then an array of `uint16` **cumulative
offsets** (rounded up to an `off_t` boundary), then an array of `off_t` values.
The lengths are not per-key bytes: entry `i` is the total length of keys `0..i`,
so a key's start is entry `i-1` and its length the step between them -- exactly
what Haiku's `_InsertKey` (`length + previous`) and `bplustree_node::KeyAt`
(`lengths[i] - lengths[i-1]`) write and read. A node is a leaf exactly when
`overflow_link == BPLUSTREE_NULL`; an internal node's values are child node
blocks. `BPLUSTREE_FREE` (`-2`) marks a free node.

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
`type_code` (`TypeCodeToKeyType`). Keys compare by their type: a string
bytewise, an integer or real as a number, so an int64 index holds its keys in
numeric order as Haiku's does. A tree may allow duplicate keys; those are held
in a **duplicate array** reached through `overflow_link`, with fragment nodes
past that (`NUM_DUPLICATE_VALUES` 125, `NUM_FRAGMENT_VALUES` 7). Aegir writes
only the duplicate-**node** form — a whole node holding `{count; values[]}`
at its overflow link, chained by the sibling links when it fills — never the
fragment form, which Haiku nonetheless reads; Haiku reads and appends to the
node form either way. The name index allows duplicates, because two
directories may hold the same name.

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

`mkfs` now creates the four indices: the indices directory (mode
`S_INDEX_DIR | S_STR_INDEX | S_DIRECTORY | 0700`, its parent its own run),
four index inodes under it (mode `S_INDEX_DIR | S_DIRECTORY | S_<type>_INDEX`,
carrying the Be `type_code` `'CSTR'` or `'LLNG'`, and no file-name small data),
and their entries. The rules are Haiku's (`Inode::InNameIndex` and friends):
the **name** index takes every regular named inode — files and directories,
but not the root, the indices directory, or attributes — and allows
duplicates, because a name repeats across directories; the **size** index
takes files; the **last_modified** index takes files and symlinks. An index
inode's key type is its mode's `S_<type>_INDEX` bit (`BPlusTree::ModeToKeyType`),
so `name` and `BEOS:APP_SIG` read bytewise and `last_modified` and `size` read
their int64 keys numerically. Repeated keys go in a duplicate node, so the
mkfs `last_modified` index holds every inode at time zero under the one key.

The Writer maintains the name, size and last_modified indices: creating an
inode adds it to the name index (and, for a file, the size and last_modified
indices at its size and time), removing it takes those entries away, renaming
changes only the name key, and writing or truncating moves the size and
time keys. The `BEOS:APP_SIG` index follows that attribute: a write that
starts at its beginning replaces the old value's key with the new value's
(up to `MAX_INDEX_KEY_LENGTH` 255 bytes), and removing the attribute drops
the key. A directory is in the name index but not in size or last_modified,
as Haiku's `Inode::InSizeIndex` and `InLastModifiedIndex` decide. This is
also why `mkfs` populates the indices rather than leaving them empty: a
Haiku mount trusts a volume's indices, so an index present but stale is worse
than one absent. A zero `indices` run is Haiku's own "no index directory".

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
of the volume; `log_start` and `log_end` are **block positions within that
run** (0..log_length), and the superblock's `flags` is `'DIRT'` while entries
are pending and `'CLEN'` when the volume is clean.

A transaction writes, in order:

1. One or more **run array** blocks. A `run_array` is a block-sized header —
   `int32 count; int32 max_runs; block_run runs[]` — listing, in ascending
   block order, the target blocks the transaction changed. Its size is the
   block size, so a freshly allocated one is ready to use; the `max_runs`
   field is capped at 127 whatever the block size (Haiku's
   `run_array::MaxRuns`) and its usable count is one less, for an off-by-one
   in Be's implementation. **Be's replay can only expand runs of length one**,
   so every run is a single block.
2. Immediately after each array block, the **data blocks** for its runs, in
   order. So the log is a flat stream of `[array][data…][array][data…]`.

Commit is the superblock write that sets `flags = 'DIRT'` and `log_end` to the
position past the entry, then flushes the device. On mount, **replay** reads
entries from `log_start` to `log_end` and copies each run's data to its target
run; then it sets `log_start = log_end` and the volume clean. A clean volume
has `flags == 'CLEN'` and `log_start == log_end`, so replay is a no-op — the
**clean-volume fast mount**: no scanning, no replay.

The log records **metadata only** — inodes, directory and attribute B+tree
nodes, the allocation bitmap and the superblock — the same choice BeOS made.
File data is never journalled, so a file written when the machine dies may
lose its tail, but the filesystem's own structures stay consistent.
`AEGIR_BFS_JOURNAL_FULL` marks a volume whose data blocks are journalled too;
it is not set.

One operation is one transaction. An operation reads back blocks it wrote
earlier in the same transaction (a run inserted into an indirect array is read
again to place the next run), so the transaction buffers the blocks it changed
and the volume reads through the buffer until commit. The buffer holds
`kMaxJournalBlocks` blocks; a metadata operation that needs more is refused
rather than committed in pieces. Commit buffers the whole entry, writes it to
the log, marks the volume dirty, then checkpoints the blocks to their homes
and retires the log — so a crash leaves either nothing or a log that replay
repairs.

## The service and the library

The implementation is split the way the repository is (`AGENTS.md`), and no
file is a monolith:

- `libs/freestanding/aegir-bfs/` — the format and its algorithms, usable
  without a libc: `layout.h` (the structs and byte order), `volume.{h,cc}`
  (superblock, block runs, block I/O through the client window),
  `inode.{h,cc}`, `bplustree.{h,cc}`, `allocator.{h,cc}`,
  `writer.{h,cc}`, `attribute.{h,cc}`, `index.{h,cc}`, `query.{h,cc}`,
  `journal.{h,cc}`.
- `apps/freestanding/aegir-fs-bfs/` — the volume service: bootstrap, announce,
  and the serve loop, in `aegir-fs-fat`'s shape.
- `libs/freestanding/aegir-metadata/` — the metadata and query protocol shared
  by the service, the VFS client and the runtime.
- `scripts/mkfs_bfs.py` — the host-side builder for the test disk.

The write phase lands in steps, and each smaller step is a completed
implementation rather than a stub. Directories grow: a full leaf splits in
half, the separator rises into its parent, and a parent that splits too makes
a new root, so a directory has no one-node bound and the listing cursor walks
the leaves. A stream grows through its direct runs, then an indirect array,
then the double indirect, whose runs are laid out in units of its own block
length (`_DoubleIndirectBlockLength()`), exactly as Haiku computes them. A cut
that lands inside the double indirect -- unreachable for a stream that fits
the single indirect -- is refused rather than miswritten. Growth is non-sparse:
a byte the file grows across is a real zeroed block, and a write past the end
zero-fills the gap. Every operation leaves the volume `'CLEN'` with
`log_start == log_end`.

The Aegir additions are **runtime features** wherever possible, because the
format need not change for them: TRIM is a discard on a freed run, permission
enforcement is a check of the caller's badge against the inode's stored `uid`,
`gid` and `mode`, and the clean fast mount is a decision not to replay. Only
sparseness changes what a read returns, so only sparseness takes a gate bit.

## Metadata over the volume protocol

The volume protocol (`specs/vfs.md`) grows methods for attributes and queries,
defined in `libs/freestanding/aegir-metadata/` (`aegir/metadata.h`) and
numbered `12` upward so a version that does not know them answers by saying
nothing. The base methods answer refusals with an empty reply; metadata needs
to tell "this filesystem has no such attribute" from "this filesystem has no
metadata", so the new methods answer a **status word first**:

| Value | Name | Meaning |
| ----- | ---- | ------- |
| 0 | `kOk` | the call succeeded |
| 1 | `kUnsupported` | the filesystem does not implement this; the runtime maps it to `EOPNOTSUPP` |
| 2 | `kNotFound` | no such path, or no such attribute |
| 3 | `kInvalidName` | the name is empty or longer than 255 bytes |
| 4 | `kReadOnly` | the volume is read-only |
| 5 | `kNoSpace` | the volume is full |
| 6 | `kNotADirectory` | the path names something that is not a directory |
| 7 | `kIsADirectory` | the call wanted a file and the path names a directory |

FAT answers **`kUnsupported`** to every metadata method; it never pretends.
The runtime maps `kUnsupported` to `EOPNOTSUPP`, `kNotFound` to `ENODATA`, and
so on.

The methods mirror Haiku's `fs_*attr` API:

- **attr stat** — in: path words, name. Answer: status, `type_code`, size.
- **attr read** — in: path words, name, offset, max. Answer: status, count,
  bytes.
- **attr write** — in: path words, name, `type_code`, offset, count, bytes.
  Answer: status, written. Creating an attribute sets its type; writing it with
  a different type is a refusal.
- **attr remove** — in: path words, name. Answer: status.
- **attr list** — in: path words, index. Answer: status, name, `type_code`,
  size, or `kNotFound` past the last attribute. The cursor is the caller's
  index, the `list`/describe pattern.

Paths are the component path from `specs/vfs.md`, resolved against the root
inode through the directory trees. A path that names a file whose inode has no
attribute directory is `kNotFound`, not `kUnsupported` — the filesystem *has*
metadata, this file simply has none.

The runtime maps the metadata protocol onto the POSIX xattr calls, so a hosted
program reaches it through musl's `getxattr`/`setxattr`/`listxattr`/
`removexattr` and their `f`- and `l`-forms (and the freestanding `aegir::vfs`
client reaches it directly, carrying the status word). The status word becomes
an errno: `kOk` success, `kNotFound` `ENODATA`, `kUnsupported` `EOPNOTSUPP`,
`kInvalidName` `ERANGE`, `kReadOnly` `EROFS`, `kNoSpace` `ENOSPC`. A `getxattr`
with no buffer returns the size; one with too small a buffer is `ERANGE`. A
value larger than one envelope is read and written at successive offsets.

Beside the raw protocol is a **typed layer**: `aegir/metadata.h` carries the
little-endian codecs for the type codes, and `aegir::vfs::Volume` has typed
access (`attr_set_int32`, `attr_get_string`, `attr_get_raw`, ...) that stores
the matching `type_code` and, on a read, refuses an attribute whose stored
type or size is not the one asked for. A client then asks for an `int32` and
gets one, without assembling the bytes by hand.

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

A query that is a **single equality** on one of the four standard indexed
attributes is answered from that index: the literal becomes the index key --
a string attribute's bytes, or an int64's little-endian coding, the same
bytes the Writer keys it with -- and the index is walked for the inodes that
key maps to, following a duplicate chain when several share the key. Each
inode the walk names is still read back and tested against the expression,
so a stale entry cannot make the query lie. A compound query, an inequality,
an attribute with no index, or an index the volume does not have falls back
to the scan, which is always the correct answer.

- **query open** — in: the query string, flags. Answer: status, a handle.
- **query next** — in: the handle. Answer: status, one entry (name, kind,
  size), or `kNotFound` at the end. The cursor is the filesystem's, per
  handle.
- **query close** — in: the handle. Answer: status.

The evaluator scans the volume: it walks the inodes block by block, keeps the
named ones (a directory's inode carries its name in its small data, as a
file's does), and tests each against the parsed expression. An inode that
names a block other than where it was read -- a stale copy in the log, or in a
freed block not yet overwritten -- is not an inode and is skipped, which is
what keeps the scan honest without an index. `name`, `size` and
`last_modified` are the standard attributes; another name is read through the
volume's metadata layer. The literal's type and the attribute's `type_code`
decide how they compare: a string as bytes, an integer as a number, a real as
a number, a bool as a bool; a literal and an attribute of different families
do not match.

Whose names a query answers is not the query's to decide: an inode whose name
the caller could not read by path is not a result. Both the index walk and the
scan check, before they name an inode, that the caller may read the directory
holding it and execute through every directory above it to the volume root --
the same traversal `walk` enforces (`specs/ownership.md`). A query of a public
volume therefore reaches the public names and the caller's own, and a
directory a caller may not enter keeps its contents out of the answer as it
keeps them out of a listing. The scan reaches by block number what the walk
reaches by path, and this is where the two are made to agree. The system class
sees everything. An empty answer because everything matching was hidden reads
the same as one that matched nothing, and that is deliberate: the names are
what the guard is for.

A **live query** is a query that sees changes after it opens. Its open carries
a token; the filesystem answers with a **notification endpoint** of its own,
which it signals. The filesystem owns the endpoint, so the client needs no
capability-transfer right of its own:

- **query open live** — in: the query string, flags, token. Answer: status,
  handle, and one cap: the notification endpoint the filesystem made for this
  query (a read-only copy the client waits on).
- When a change (create, remove, rename, attribute write) makes an inode enter
  or leave a live query's set, the filesystem **signals** that endpoint. The
  signal is one-way (`seL4_Send`, non-blocking on a notification), so the
  filesystem's serve loop never blocks on a subscriber and a subscriber
  blocked in a write cannot deadlock it. The subscriber re-reads with query
  next, which reports the current set.

The signal carries no data; the endpoint is the query's identity. The
filesystem retypes a notification from the small untyped the partition manager
gave it and mints the client a read-only copy; it signals its own on every
change that could affect an inode -- a create, remove, rename, write, truncate
or attribute write -- whether or not that inode ends up in or out of the
query's set, and the client finds the change by re-reading and comparing. A
live query whose read has not caught up with the latest change starts its next
read from the beginning, so a read always shows the whole current set. When
the last live query closes, the filesystem revokes the untyped, reclaiming the
endpoints -- and a client's copy, should the client forget it. A live open the
filesystem has no endpoint for is refused (`kNoSpace`).

## Permissions

`uid` and `gid` are the Aegir user index of the owner, and a user is its own
group (`specs/ownership.md`); a system badge is the superuser. The `mode`'s low
nine bits are what a user's calls are checked against, and the mapping is
POSIX's:

- read a file's bytes: `r`; list a directory: `r`; stat, and traverse any
  component on the way to a name: `x`;
- open for writing, truncate, or write: `w` on the file, and `w` with `x` on
  the directory a new name is made in;
- mkdir, remove and rename: `w` with `x` on the directory that holds the name;
- an attribute's read, stat and list: `r`; its write and remove: `w`;
- `Protect` and `Owner`: the inode's owner or the system class alone, which is
  an ownership, not a mode bit.

A directory a caller may not traverse refuses before the walk descends, which
is what keeps a user out of another user's home even when a file inside it is
world-readable. `Protect` sets the low permission bits and leaves the type and
the extended bits alone; `Owner` sets `uid` and `gid`. A new inode is owned by
its creator -- the caller's user index, or the system's -- and a file is born
`0644` and a directory `0755`. FAT answers `kUnsupported` to both, and
`kPermission` is the status when a caller may not touch an attribute.

## Sparseness

A sparse file has holes: ranges of zeros with no blocks behind them. BFS's
extent model has no hole marker -- a zero `block_run` ends the run list -- so a
*hole in the middle* is not representable without an extension, and none is
taken. Aegir's sparseness is therefore the **tail**: the inode's logical size
may exceed the bytes its runs cover, and a read of the uncovered tail returns
zeros with no block behind it. Truncating to a larger size sets the size and
allocates nothing; a write past the old end allocates only the range it
writes, and a *seek-forward* write makes its gap real zeroed blocks, because a
run's position follows the runs before it. Appending after a sparse tail
likewise fills the tail with real blocks rather than leaving a middle hole.

This needs no format change and no gate bit: a volume Aegir writes remains one
Haiku reads, because there are no zero runs in the middle of a stream. A file
whose size exceeds its runs is read by Aegir's own reader as zeros; the run
list simply ends. Phase 3 implements this; `AEGIR_BFS_SPARSE` is not used.

## Extensions and their gates

| Extension | On-disk change | Gate |
| --------- | -------------- | ---- |
| TRIM on free | none | a device that answers `discard` (below) |
| Sub-second times | none (Haiku's own encoding) | a clock finer than a second |
| Clean no-replay fast mount | none | `flags == 'CLEN'` and `log_start == log_end` |
| Permission enforcement | none (uid/gid/mode are already stored) | Aegir badges (`specs/authority.md`); with the home (`specs/ownership.md`) |
| Tail sparseness | none (a size past the runs) | none |
| Full data journaling | none | `AEGIR_BFS_JOURNAL_FULL` |

### TRIM

The block protocol (`specs/services.md`) grows a **caps** method (5) and a
**discard** method (6):

- **caps** — no words in, an answer of two: `flags`, whose bit 0 is "discard is
  supported", then `max_discard_sectors`, the largest range one discard may
  name. It crosses in the reply rather than in a window because a window
  belongs to whoever started the caller and the driver maps only its own
  (`libs/freestanding/aegir-block/include/aegir/block.h`), so a filesystem
  asking on mount has no window the driver could write. `Identify` keeps its
  fixed 24-byte wire and is not extended.
- **discard** — words: first sector, sector count; clamped by the caller's
  badge to its range, as read and write are, though not by the window, since
  nothing crosses it. The driver translates it to the device's unmapped-range
  command, splitting by the device's own bound and dropping partial edge
  sectors it cannot align. A device that does not support discard answers zero,
  and the filesystem stops asking.

The virtio-blk driver negotiates `VIRTIO_BLK_F_DISCARD` (feature bit 13) and
issues `VIRTIO_BLK_T_DISCARD` requests; the QEMU drive is given
`discard=unmap` in `scripts/targets.py`, since without it the backend does not
advertise the feature. BFS discards a run when it frees it, in
`Allocator::free`'s shape: adjacent frees coalesce into one run, and the run is
handed to the device only once the transaction that freed it has committed --
a discard before the commit would throw away blocks an abort still needs.

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
- **A live query hears every change, not only the ones it may see.** Any change
  (create, remove, rename, write, truncate, attribute write) signals every open
  live query's endpoint without asking whether the changed inode is one the
  subscriber may reach, so a subscriber learns that *something* changed the
  volume -- not what, and not whose. The name-level guard in `Queries` keeps
  the answer itself clean, so this is a timing side channel rather than a leak
  of names; closing it means tracking, per query, which changed inodes entered
  or left that caller's reachable set, which a one-way signal with no payload
  cannot carry. Recorded here, not fixed.

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
3. **Sparseness is the tail only** -- a size past the runs, read as zeros --
   and needs no gate. A middle hole would need a zero run Aegir could read
   past but Haiku could not, and the trade was not taken.
4. **The block capabilities are a new method**, not a wider `Identify`, so the
   partition manager's fixed wire and every existing caller are untouched.
5. **Live queries signal a one-way endpoint and re-read; they do not carry the
   change.** This keeps the single-threaded serve loop and makes deadlock
   impossible; the cost is a re-read per notification. The filesystem owns the
   endpoint (its answer carries a read-only copy), so a client needs no
   capability-transfer right and the signal can never block on a cap whose type
   the filesystem cannot check.
6. **The filesystem registry is data**, one row per GPT type GUID mapping to a
   kind prefix and an initrd binary, and the device manager hands the
   partition manager a named bundle of the helper images rather than one. This
   is `specs/services.md`'s business and lands with Phase 1.
7. **Permission enforcement is a badge check against the inode, and it lands
   with the home.** A user badge (`specs/authority.md`: bit 62 set) carries
   its user row index in bits 24..61; that index is the inode's `uid` and `gid`
   alike -- a user is its own group -- so the owner and group triads of `mode`
   both key off it, and the other triad applies to every caller it does not
   match. A system badge (bit 62 clear) is the superuser and passes every
   check. The calls that change a mode or an owner are the AmigaDOS pair
   **`Protect`** and **`Owner`**, not `chmod`/`chown`; they are the filesystem's
   half of the home, whose namespace half is the view `specs/ownership.md`
   defines. Both halves land together, because the view alone leaves the home
   reachable through the public `Sys:` path and the ownership alone leaves the
   home root-owned.
