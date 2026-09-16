# virtio over MMIO — reference

External reference material, kept in-tree so it can be cited the way `kernel/manual` is.
**This is not Aegir's specification.** Aegir's own decisions about drivers are in
`specs/services.md`; this file is what those decisions were checked against.

## Provenance

- **The OASIS virtio specification, 1.x.** The MMIO transport chapter, including the legacy
  interface. Fetched from
  `https://raw.githubusercontent.com/oasis-tcs/virtio-spec/efd4028b7aec5030b6ac39c0ddc50353d0658704/content.tex`
  (commit `efd4028b`), 2026-09. The legacy interface is **non-normative** in 1.x — VIRTIO-110
  removed its MUSTs — which is part of why it is worth reading rather than recalling.
- **Linux's `drivers/virtio/virtio_mmio.c`**, the reference implementation of the legacy
  sequence. Quoted from
  `https://android.googlesource.com/kernel/common.git/+/66846048f55c6c05a4c46c2daabb773173f8f28d/drivers/virtio/virtio_mmio.c`
  (blob `2f57380d`), 2026-09.
- **`projects/util_libs/libvirtio`** and **`projects/sel4_projects_libs/libsel4vmmplatsupport`**
  in this repository carry the ring structures and the device side of the same ABI.

## The transport's registers

All 32-bit. The table is Linux's, which is the legacy interface; the modern interface adds the
64-bit queue address registers and a ready bit, and is otherwise the same map up to 0x040.

| Offset | Dir | Name | Meaning |
|---|---|---|---|
| 0x000 | R | MagicValue | `virt` = 0x74726976 |
| 0x004 | R | Version | 1 = legacy, 2 = modern |
| 0x008 | R | DeviceID | 0 = nothing behind the transport |
| 0x00c | R | VendorID | |
| 0x010 | R | HostFeatures | read with HostFeaturesSel |
| 0x014 | W | HostFeaturesSel | |
| 0x020 | W | GuestFeatures | written with GuestFeaturesSel |
| 0x024 | W | GuestFeaturesSel | |
| 0x028 | W | GuestPageSize | guest page size in bytes; written before any queue is used |
| 0x030 | W | QueueSel | which queue the next queue registers are about |
| 0x034 | R | QueueNumMax | **0 means the queue is unavailable** |
| 0x038 | W | QueueNum | the queue's size, must be ≤ QueueNumMax |
| 0x03c | W | QueueAlign | Used Ring alignment, in bytes, for the selected queue |
| 0x040 | RW | QueuePFN | page number of the queue's first page; 0 is illegal/inactive |
| 0x050 | W | QueueNotify | write the queue index to kick it |
| 0x060 | R/W | InterruptStatus / InterruptACK | same offset, read then write back |
| 0x070 | RW | Status | writing 0 resets the device |
| 0x100+ | RW | config | device-specific |

**Modern-only** (not present in the legacy map): 0x044 QueueReady, 0x080/0x084 QueueDescLow/High,
0x090/0x094 QueueDriverLow/High, 0x0a0/0x0a4 QueueDeviceLow/High.

## The legacy configuration sequence

From the specification, and visible in `vm_setup_vq`:

1. Select the queue: write its index to `QueueSel`.
2. **Read `QueuePFN`, expecting 0** — nonzero means the queue is already in use.
3. **Read `QueueNumMax`; zero means the queue is unavailable.**
4. Allocate **and zero** the queue pages, contiguous, with the Used Ring aligned. Linux uses
   `__GFP_ZERO` for exactly this reason.
5. Write the size to `QueueNum` (≤ `QueueNumMax`).
6. Write `QueueAlign` — Linux hardcodes it to the page size.
7. Write `QueuePFN = physical >> PAGE_SHIFT` — **this activates the queue.** Writing 0
   deactivates it again, and a device reset clears it.

`GuestPageSize` is written once, before any of this: the device multiplies `QueuePFN` by it to
get the queue's address.

## The legacy ring layout

`Descriptor Table | Available Ring | padding | Used Ring`, the padding being whatever brings the
Used Ring to a multiple of `QueueAlign`. `vring_init` in `projects/util_libs/libvirtio`
expresses it exactly:

    vr->used = align(&vr->avail->ring[num] + sizeof(uint16_t), align)

so with `num` = 8 and `align` = 4096 the Used Ring is at **4096** — a second page. A queue whose
Used Ring does not land there is a queue the device writes its completions past.

## What this cost us

Recorded because it is the point of keeping the file. The legacy queue was written from *recall*
of these tables: the offsets were shifted by one register, the queue size went into a read-only
register, and `QueueNumMax` was read at the wrong address and its zero read as "wrong offset"
rather than "queue unavailable". Six commits of measurement followed, every one of which this
section would have replaced.
