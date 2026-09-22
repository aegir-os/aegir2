# The object allocator: a buddy over untypeds

Status: decided (2026-09). This is the spec `libs/aegir-mem`'s untyped
management lands under.

`specs/userland.md` records the allocator's history: it was written as the root
task's, allocation-only, and deliberately without the `libsel4utils` dependency.
It has since grown callers (the spawner, auth's session pool, the console's
slices, `aegir-heap`) and a real bug: splitting an untyped to fit an object
fails under some patterns with `seL4_NotEnoughMemory`, from the homegrown
`split_to` bookkeeping. `libsel4allocman`'s `src/utspace/split.c` is the tested
shape for exactly this, and it is already vendored
(`projects/seL4_libs/libsel4allocman`, BSD-2-Clause). This spec replaces our
splitting with that shape.

## The decisions

- **The untyped pool is a buddy allocator.** Free pieces are held in one list
  per size (a power of two), not in a flat record table. A piece is a **node**:
  its capability, its size, its physical base, the node it was split from
  (`parent`), its **buddy** (`sibling`), and the free list it is in (`head`, or
  null when allocated). This is `utspace_split_node`'s shape, and it is what
  makes free possible at all.
- **Refilling is a buddy split.** To obtain a piece of `size_bits`, take one
  from a larger list and split it into two equal halves (two `Untyped_Retype`
  calls of `size_bits` each), insert both, and repeat until a piece of the
  wanted size exists. This is `_refill_pool` exactly: the two halves are
  buddies, and the parent is kept so a later free can merge them back.
- **Allocation pops and retypes; the node is consumed.** `alloc_object` refills
  the wanted size, takes a node, retypes the object out of its capability, and
  the node's storage is released (its memory is the object now).
  `carve_untyped` refills, takes a node, and hands its capability out whole.
- **Free merges buddies.** Freeing a node puts it back; if its buddy is also
  free, both are deleted and their parent is freed in turn, up to the root.
  This is `_utspace_split_free`. It needs the node, so the allocation API grows
  a **cookie**: `alloc_object` (and the others) can return the node, and
  `free(cookie, size_bits)` takes it back. The capability a caller holds is
  *not* the cookie -- a node's capability is the piece of memory, an object's
  capability is what was retyped from it, and only the allocator knows which
  node an object came from.
- **Two classes, as the boot describes them.** Normal untypeds and device
  untypeds have separate free lists (`heads`, `dev_heads`), because a device
  frame can only be retyped from the device untyped that covers its address
  (`device_window`'s rule). A `dev_mem` class (device memory that may hold
  kernel objects) is not used today and is not carried.
- **Node storage is provided and grown, not a fixed table.** A split adds one
  node, and the pool has to hold the whole buddy tree -- which a static array
  cannot size well for both the 2 GiB root task and a 2 MiB service, and which
  made every service carry a table it would never touch. So the allocator does
  not own its nodes: it is *given* mapped regions (`adopt_nodes`) and asks its
  caller for another when it runs low. The caller is the one that can carve and
  map (it has the `Scratch` and the untyped); the allocator only retypes from
  untypeds it already holds and cannot map its own bookkeeping. Growth is
  bounded by the untyped the process was given, so it is not unbounded -- a
  service handed 2 MiB can never build more than 2 MiB of nodes. This is the
  shape allocman uses (an mspace over a caller-provided pool), minus the mspace.
- **The allocator's bookkeeping is a function of its grant, not a constant.**
  The node region a caller hands over is sized from the grant the allocator
  manages, so a service does not get a number chosen for the root task. The
  same idea retires the other fixed sizes this project has been guessing at
  (`stack_kib`, `delegate_mib`): where a value is derivable from the grant, it
  should be derived.
- **The public API does not change shape.** `alloc_object`, `carve_untyped`,
  `carve_page`, `device_window`, `adopt_untyped`, `adopt_slots`, `make_asid_pool`
  keep their contracts; the buddy is the internals. `alloc_object` and
  `carve_untyped` grow an optional `void **cookie` out-parameter for the free
  that follows.
- **`device_window` keeps its rule.** A device frame is reached by retyping
  every page before it from the device untyped that covers the address, and the
  pages along the way are kept. The device free lists are how those kept pages
  are remembered rather than re-retyped.

## The shape

### A node

    struct Node {
        seL4_CPtr cap;        // this piece's untyped capability
        uint64_t physical;    // its physical base, or 0 when the giver did not say
        uint8_t size_bits;    // its size
        uint8_t device;       // device untyped or normal
        Node *parent;         // the node split to make this one, or null
        Node *sibling;        // its buddy, or null for a root
        Node **head;          // the free list it is in, or null when allocated
    };

The lists are `Node *heads[seL4_WordBits]` and `Node *dev_heads[seL4_WordBits]`,
indexed by `size_bits`. A node is in exactly one of them or allocated.

### The operations

- **add** -- a root node (an untyped the boot or a giver handed us) goes into
  its size's list, in physical order so that allocations come off in address
  order (allocman's reason: contiguous physical memory is friendlier to
  devices).
- **refill(size)** -- while the list is empty, refill a larger size and split
  its first node into two `size`-wide buddies.
- **alloc(type, size_bits)** -- refill `object_bits(type, size_bits)`, take a
  node, retype the object from `node->cap`, release the node's storage.
- **free(cookie, size_bits)** -- insert the node; if `sibling` is free, delete
  both and free `parent`.
- **carve(size_bits)** -- refill, take a node, hand out `node->cap`.

### What is Aegir's and what is allocman's

Allocman's file is the algorithm and the node shape; it is C, it allocates
nodes from an mspace, and it takes its capabilities through `vka`. Aegir keeps
its own API, its own static node pool and its own slot cursor; what is ported is
the buddy, not the library OS (`specs/userland.md`). The port is credited in the
file, with the upstream path and its licence.

## Future: memory auditing

Allocators are where memory bugs hide, and this arc showed it: a service's
untyped, its node pool, its CSpace slots and its heap all have budgets that
today are either constants or derived, and nothing reports what is actually
used. A later milestone adds **auditing**: every allocator and the spawner
report what they hold and what they spend -- untyped bytes by class, nodes
live and free, slots used, per-account charges -- and the boot report and a
queryable port expose it. The goal is to make a "no memory" failure say *which*
budget ran out, and to let a person watch a running system's memory instead of
inferring it from a fault.

This is deliberately a milestone, not a side quest: the accounts
(`specs/authority.md`) already charge every allocation, so the data exists; what
is missing is the reporting and the queries.

## What this is not

Freeing a *slot* (a capability) -- still the deferral `specs/userland.md`
records; this is freeing the *memory* behind an object. A `dev_mem` class.
Growing the CSpace. Freeing the root untypeds the boot handed us.

## Acceptance

The boot and its acceptance are unchanged (`make run`, `AEGIR_BOOT_OK`), and
the env-smoke that found the bug now boots (`ENV_SMOKE_OK`). A unit test in
`aegir-test` splits a large untyped into many objects, frees half of them, and
allocates again -- the buddy must merge and hand the same physical memory back
-- and a boot with the free path exercised must not leak nodes (the pool
returns to its pre-allocation count).
