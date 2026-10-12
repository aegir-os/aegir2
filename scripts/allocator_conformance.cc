/*
 * Host conformance for aegir-mem's allocator (specs/memory.md).
 *
 * The allocator's splitting, buddy merge and free lists are pure bookkeeping;
 * the only thing it needs from the kernel is the rule that a retype from a
 * spent untyped is refused (scripts/allocator_stub/sel4/sel4.h models it). The
 * driver runs a long randomized allocation workload over that model and fails
 * the moment a retype is refused -- the "Untyped Retype: Insufficient memory"
 * the launcher's spawn printed, at a cost of seconds rather than a QEMU boot.
 */

#include <sel4/sel4.h>

#include <aegir/mem/allocator.h>
#include <aegir/mem/frame_region.h>
#include <aegir/mem/slot_pool.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

unsigned g_checks = 0;
unsigned g_failures = 0;

void expect(bool ok, char const *what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

uint64_t g_state = 0x9e3779b97f4a7c15ull;

uint64_t rng()
{
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return g_state;
}

struct Live {
    seL4_CPtr cap;
    void *cookie;
    seL4_Word wanted;
    /* A carved untyped is a *piece* the allocator still owns: the caller hands
     * it back with its cap intact (aegir-console revokes the objects retyped
     * from it and does not delete the cap). An object the caller retyped is the
     * caller's own, and it deletes that before giving the piece back. */
    bool carve = false;
};

/* The untyped source: a memory service handing out 21-bit chunks, bounded the
 * way the service's own pool is -- past its ceiling it answers nothing and an
 * allocation fails, rather than growing forever. */
constexpr unsigned kChunks = 64;
unsigned g_chunks = 0;
/* The source's chunk width. The memory service hands out 21 bits; a test may
 * widen it to force a split deeper than the slot reserve, which is the
 * invariant under test. */
unsigned g_chunk_bits = 21;

seL4_CPtr source(void *, seL4_Word want_bits, seL4_Word *bits, uint64_t *paddr)
{
    if (g_chunks >= kChunks) {
        return 0;
    }
    /* A grown chunk arrives like the memory service's: a size and no address
     * (`command_untyped_source` answers zero), so the allocator has no order to
     * keep for it. Only the seed the process was started with has one. The
     * memory service honors the requested width (aegir-memory's alloc handler),
     * so `want_bits != 0` is a request for exactly that. */
    *paddr = 0;
    ++g_chunks;
    unsigned const width =
        want_bits != 0 ? static_cast<unsigned>(want_bits) : g_chunk_bits;
    *bits = width;
    return host_sel4::make_root_untyped(width);
}

/* What the allocator is asked for: the shapes a spawn's staging uses -- CNodes,
 * pages, TCBs, endpoints, notifications -- whose wanted sizes are 4..17 bits. */
struct Shape {
    seL4_Word type;
    seL4_Word size_bits;
};

constexpr Shape kShapes[] = {
    {seL4_CapTableObject, 8},  {seL4_CapTableObject, 9},  {seL4_CapTableObject, 10},
    {seL4_CapTableObject, 11}, {seL4_CapTableObject, 12}, {seL4_RISCV_4K_Page, 12},
    {seL4_TCBObject, 10},      {seL4_EndpointObject, 4},  {seL4_NotificationObject, 5},
};

aegir::mem::Account g_account{"workload", 0, 0, 0};

/* The launcher's node source is vspace.cc's `grow_nodes_from_window`: to grow
 * the allocator's own bookkeeping it retypes a frame ... by calling the very
 * allocator it belongs to. This mimics that -- an `alloc_object` from inside
 * the node source -- so the harness can reproduce the re-entrant refill. */
struct NodeWindow {
    aegir::mem::Allocator *allocator;
    unsigned char *pool;
    unsigned capacity;
    unsigned used;
};

void *reentrant_nodes(void *context, unsigned *bytes)
{
    auto *const window = static_cast<NodeWindow *>(context);
    seL4_Error error = seL4_NoError;
    void *cookie = nullptr;
    /* The frame the bookkeeping lives in is an object this allocator must
     * retype; the call re-enters refill while the outer refill holds a parent
     * it has chosen but not yet unlinked. */
    (void)window->allocator->alloc_object(seL4_RISCV_4K_Page, seL4_PageBits, g_account, &error,
                                          &cookie);
    unsigned const chunk = 4096;
    if (window->used + chunk > window->capacity) {
        *bytes = 0;
        return nullptr;
    }
    unsigned char *const region = window->pool + window->used;
    window->used += chunk;
    *bytes = chunk;
    return region;
}

/* A node pool too small for a single split, so the very first `alloc_node`
 * inside `refill_inner` asks the source -- and the source re-enters. Returns
 * true when nothing was refused and the lists stayed consistent. */
bool reentrancy_holds()
{
    host_sel4::reset();
    g_chunks = 0;
    seL4_BootInfo info{};
    info.empty = {1, 8000};
    info.initThreadCNodeSizeBits = 12;
    seL4_CPtr const root = host_sel4::make_root_untyped(22);
    info.untypedList[0].sizeBits = 22;
    info.untyped = {root, root + 1};

    aegir::mem::Allocator allocator(&info);
    if (!allocator.initialise()) {
        return false;
    }
    allocator.set_untyped_source(source, nullptr);
    alignas(64) static unsigned char nodes[512];
    allocator.adopt_nodes(nodes, sizeof(nodes));
    alignas(64) static unsigned char pool[1 << 20];
    NodeWindow window{&allocator, pool, sizeof(pool), 0};
    allocator.set_node_source(reentrant_nodes, &window);

    unsigned const before = host_sel4::refusals();
    for (unsigned i = 0; i < 80; ++i) {
        seL4_Error error = seL4_NoError;
        void *cookie = nullptr;
        (void)allocator.alloc_object(seL4_RISCV_PageTableObject, seL4_PageTableBits, g_account,
                                     &error, &cookie);
    }
    return host_sel4::refusals() == before && allocator.check_free_lists() == 0;
}

/* The console's slice flow (specs/console.md): carve a big untyped for a
 * client's slice, then retype the slice's frames from that cap. The node source
 * is the re-entrant one and the node pool is small, so each carve re-enters
 * `refill` while a parent is held. Returns true when nothing was refused. */
bool carve_and_frames_hold()
{
    host_sel4::reset();
    g_chunks = 0;
    seL4_BootInfo info{};
    info.empty = {1, 8000};
    info.initThreadCNodeSizeBits = 12;
    seL4_CPtr const root = host_sel4::make_root_untyped(24);
    info.untypedList[0].sizeBits = 24;
    info.untyped = {root, root + 1};

    aegir::mem::Allocator allocator(&info);
    if (!allocator.initialise()) {
        return false;
    }
    allocator.set_untyped_source(source, nullptr);
    alignas(64) static unsigned char nodes[512];
    allocator.adopt_nodes(nodes, sizeof(nodes));
    alignas(64) static unsigned char pool[1 << 20];
    NodeWindow window{&allocator, pool, sizeof(pool), 0};
    allocator.set_node_source(reentrant_nodes, &window);

    constexpr unsigned kSliceBits = 23;             /* 8 MiB, the console's chunk */
    constexpr unsigned kFrameBits = 21;             /* a RISC-V mega page */
    constexpr unsigned kFrames = 1u << (kSliceBits - kFrameBits);

    unsigned const before = host_sel4::refusals();
    for (unsigned i = 0; i < 8; ++i) {
        seL4_Error error = seL4_NoError;
        void *cookie = nullptr;
        seL4_CPtr const untyped =
            allocator.carve_untyped(kSliceBits, g_account, &error, nullptr, &cookie);
        if (untyped == 0) {
            continue;
        }
        seL4_CPtr frames[1u << (kSliceBits - kFrameBits)] = {};
        for (unsigned f = 0; f < kFrames; ++f) {
            frames[f] = allocator.carve_page(untyped, g_account, &error, kFrameBits);
        }
        /* The console reaps a slice: the frames die in the revoke of the piece's
         * cap; the cap is the allocator's and is not deleted; then the piece
         * goes back and merges with its buddy (aegir-console's reap says why the
         * cap must survive). */
        for (unsigned f = 0; f < kFrames; ++f) {
            if (frames[f] == 0) {
                return false;
            }
        }
        (void)seL4_CNode_Revoke(seL4_CapInitThreadCNode, untyped, 12);
        (void)allocator.free_object(cookie, kSliceBits);
    }
    return host_sel4::refusals() == before && allocator.check_free_lists() == 0;
}

/* The allocator's contract says a caller revokes the objects it retyped from a
 * piece before handing the piece back. It must not *depend* on that: the merge
 * revokes, so a piece handed back with its object still alive leaves no
 * orphaned memory. With the merge only deleting, the live object is left behind
 * and the parent comes back whole while its memory is still in use -- the spent
 * piece a later retype refuses with "0 bytes available". */
bool free_without_revoke_holds()
{
    host_sel4::reset();
    g_chunks = 0;
    seL4_BootInfo info{};
    info.empty = {1, 8000};
    info.initThreadCNodeSizeBits = 12;
    seL4_CPtr const root = host_sel4::make_root_untyped(22);
    info.untypedList[0].sizeBits = 22;
    info.untyped = {root, root + 1};

    aegir::mem::Allocator allocator(&info);
    if (!allocator.initialise()) {
        return false;
    }
    allocator.set_untyped_source(source, nullptr);

    unsigned const before = host_sel4::refusals();
    for (unsigned i = 0; i < 64; ++i) {
        seL4_Error error = seL4_NoError;
        void *cookie = nullptr;
        (void)allocator.alloc_object(seL4_RISCV_4K_Page, seL4_PageBits, g_account, &error,
                                     &cookie);
        if (cookie != nullptr) {
            /* The caller skipped its revoke: the page is still alive. */
            (void)allocator.free_object(cookie, seL4_PageBits);
        }
    }
    return host_sel4::refusals() == before && host_sel4::orphans() == 0 &&
           allocator.check_free_lists() == 0;
}

/* A FrameRegion is the sequence a service used to hand-roll: carve a piece,
 * retype N frames from it, and give it back whole. A caller that never touches
 * the untyped still gets frames, and the piece goes back so the next caller
 * cannot inherit a spent one (specs/memory.md). */
bool frame_region_holds()
{
    host_sel4::reset();
    g_chunks = 0;
    seL4_BootInfo info{};
    info.empty = {1, 8000};
    info.initThreadCNodeSizeBits = 12;
    seL4_CPtr const root = host_sel4::make_root_untyped(23);
    info.untypedList[0].sizeBits = 23;
    info.untyped = {root, root + 1};

    aegir::mem::Allocator allocator(&info);
    if (!allocator.initialise()) {
        return false;
    }
    allocator.set_untyped_source(source, nullptr);

    constexpr unsigned kFrames = 4;
    constexpr unsigned kFrameBits = 21;
    unsigned const before = host_sel4::refusals();
    for (unsigned round = 0; round < 4; ++round) {
        aegir::mem::FrameRegion region;
        seL4_CPtr caps[kFrames] = {};
        seL4_Error error = seL4_NoError;
        if (!region.create(allocator, g_account, 23, kFrames, kFrameBits, caps, &error)) {
            return false;
        }
        for (unsigned f = 0; f < kFrames; ++f) {
            if (caps[f] == 0 || !host_sel4::g_caps[caps[f]].valid) {
                region.release(allocator);
                return false;
            }
        }
        region.release(allocator);
        /* The release took the frames with the piece: the allocator revokes. */
        for (unsigned f = 0; f < kFrames; ++f) {
            if (host_sel4::g_caps[caps[f]].valid) {
                return false;
            }
        }
    }
    return host_sel4::refusals() == before && allocator.check_free_lists() == 0;
}

/* A ring of the allocator's piece movements, so the moment a piece is found
 * spent can be read back: what was inserted, split and taken before it. */
struct Event {
    char event[8];
    unsigned bits;
    uint64_t physical;
    bool split;
    seL4_CPtr cap;
};
constexpr unsigned kRing = 4096;
Event g_ring[kRing];
unsigned g_ring_at = 0;
unsigned g_ring_count = 0;

void dump_ring()
{
    for (unsigned i = 0; i < g_ring_count; ++i) {
        unsigned const index = (g_ring_at + kRing - g_ring_count + i) % kRing;
        std::printf("   %-6s bits %u at %llx cap %llu %s\n", g_ring[index].event, g_ring[index].bits,
                    static_cast<unsigned long long>(g_ring[index].physical),
                    static_cast<unsigned long long>(g_ring[index].cap),
                    g_ring[index].split ? "split" : "root");
    }
}

void ring_trace(void *, char const *event, unsigned size_bits, uint64_t physical,
                bool split_child, seL4_CPtr cap)
{
    Event &e = g_ring[g_ring_at];
    std::snprintf(e.event, sizeof(e.event), "%s", event);
    e.bits = size_bits;
    e.physical = physical;
    e.split = split_child;
    e.cap = cap;
    g_ring_at = (g_ring_at + 1) % kRing;
    if (g_ring_count < kRing) {
        ++g_ring_count;
    }
    /* A listed piece must be childless: the kernel resets a childless untyped's
     * free index, so only a childless piece is whole. A piece inserted with
     * children is the spent piece that later refuses a retype. */
    if (std::string(event) == "insert" && cap < host_sel4::kMaxCaps &&
        host_sel4::g_caps[cap].children != 0) {
        std::printf("-- INSERTED A NON-WHOLE PIECE: bits %u cap %llu children %u\n",
                    size_bits, static_cast<unsigned long long>(cap),
                    host_sel4::g_caps[cap].children);
    }
    if (std::string(event) == "spent") {
        std::printf("-- a piece was spent: bits %u physical %llx cap %llu split %d; history:\n",
                    size_bits, static_cast<unsigned long long>(physical),
                    static_cast<unsigned long long>(cap), split_child ? 1 : 0);
        dump_ring();
    }
}

/* The CSpace side (specs/userland.md, specs/memory.md). A two-level process
 * whose own heap fills its first L2 CNode must grow: growth adds a CNode, which
 * costs no slot, but `refill` may have to split a chunk to get the CNode's
 * piece, and *that* spends slots (`refill_inner`'s two halves per level). The
 * allocator must grow without a refusal. This is the link's
 * `cnode-refill-refused a 17 ... slots 8161 of 8167`, in seconds. */
bool two_level_growth_holds()
{
    host_sel4::reset();
    host_sel4::set_shape(4, 12);   /* small l1, the link's l2: a CNode is 17 bits */
    g_chunks = 0;
    /* A 24-bit chunk makes `refill(17)` seven levels (14 slots), past any fixed
     * reserve -- the invariant is that growth must not need a caller-sized
     * reserve at all, so the split depth is what the case varies. */
    g_chunk_bits = 24;

    aegir::mem::Allocator allocator(nullptr);
    seL4_CPtr const root = host_sel4::make_root_untyped(22);
    if (!allocator.adopt_untyped(root, 22, 0)) {
        return false;
    }
    allocator.set_untyped_source(source, nullptr);

    /* The whole first L2 CNode, starting above the fixed slots. */
    constexpr seL4_Word kFirst = 20;
    constexpr seL4_Word kL2Bits = 12;
    constexpr seL4_Word kL1Bits = 4;
    constexpr seL4_Word kL2Total = seL4_Word{1} << kL2Bits;
    allocator.adopt_slots_level_two(kFirst, kL2Total - kFirst, kL1Bits, kL2Bits, 0, true);

    /* The re-entrant node source: growing the allocator's own bookkeeping also
     * spends slots, exactly as the link's did. */
    alignas(64) static unsigned char pool[1 << 20];
    alignas(64) static unsigned char nodes[512];
    static NodeWindow window;
    window = NodeWindow{&allocator, pool, sizeof(pool), 0};
    allocator.adopt_nodes(nodes, sizeof(nodes));
    allocator.set_node_source(reentrant_nodes, &window);

    unsigned const before_refusals = host_sel4::refusals();
    unsigned const before_collisions = host_sel4::collisions();
    unsigned const before_invalid = host_sel4::invalid();
    unsigned const before_slots = allocator.slots_total();

    /* Demand more than one CNode of pages, monotonically: no freed slot is in
     * the free list to spare the split, which is the state the link refused in. */
    std::vector<Live> live;
    bool grew = false;
    for (unsigned i = 0; i < 6000 && !grew; ++i) {
        seL4_Error error = seL4_NoError;
        void *cookie = nullptr;
        seL4_CPtr const cap =
            allocator.alloc_object(seL4_RISCV_4K_Page, seL4_PageBits, g_account, &error, &cookie);
        if (cap != 0) {
            live.push_back(Live{cap, cookie, seL4_PageBits});
        }
        grew = allocator.slots_total() > before_slots;
        if (grew) {
            std::printf("growth: slots_total %llu -> %llu, used %llu, mark %llu, largest free %u bits\n",
                        static_cast<unsigned long long>(before_slots),
                        static_cast<unsigned long long>(allocator.slots_total()),
                        static_cast<unsigned long long>(allocator.slots_used()),
                        static_cast<unsigned long long>(allocator.slot_mark()),
                        allocator.largest_free_bits());
        }
    }
    return grew && host_sel4::refusals() == before_refusals &&
           host_sel4::collisions() == before_collisions &&
           host_sel4::invalid() == before_invalid && allocator.check_free_lists() == 0;
}

/* A CNode-sized allocation (17 bits) whose split from the seed is deeper than
 * the CSpace's remaining slots must still succeed: the allocator can add a CNode,
 * which costs no slot, so slot pressure has to trigger growth *before* a split,
 * never wedge it. This is the link's `cnode-refill-refused a 17 ... slots 8161 of
 * 8167` as a unit. */
bool split_under_slot_pressure_holds()
{
    host_sel4::reset();
    host_sel4::set_shape(4, 12);
    g_chunks = 0;
    g_chunk_bits = 21;

    aegir::mem::Allocator allocator(nullptr);
    seL4_CPtr const root = host_sel4::make_root_untyped(22);
    if (!allocator.adopt_untyped(root, 22, 0)) {
        return false;
    }
    allocator.set_untyped_source(source, nullptr);

    constexpr seL4_Word kFirst = 20;
    constexpr seL4_Word kL2Bits = 12;
    constexpr seL4_Word kL1Bits = 4;
    constexpr seL4_Word kL2Total = seL4_Word{1} << kL2Bits;
    allocator.adopt_slots_level_two(kFirst, kL2Total - kFirst, kL1Bits, kL2Bits, 0, true);

    /* Hold slots until few are left, without growing: the state a process
     * reaches when its mappings have spent them. The seed is still a whole
     * 22-bit piece, so a CNode's split from it is five levels (ten slots). */
    while (allocator.slots_total() - allocator.slots_used() > 6) {
        if (allocator.alloc_slot_for_growth() == 0) {
            break;
        }
    }
    unsigned const before_collisions = host_sel4::collisions();
    unsigned const before_invalid = host_sel4::invalid();

    seL4_Error error = seL4_NoError;
    void *cookie = nullptr;
    seL4_CPtr const cap =
        allocator.alloc_object(seL4_CapTableObject, 12, g_account, &error, &cookie);
    return cap != 0 && host_sel4::collisions() == before_collisions &&
           host_sel4::invalid() == before_invalid;
}

/* A plain region source: the free list's entry pool grows the same way the
 * piece tree's node pool does, and the heap wires the same window mapper to
 * both. */
void *plain_nodes(void *context, unsigned *bytes)
{
    auto *const window = static_cast<NodeWindow *>(context);
    unsigned const chunk = 4096;
    if (window->used + chunk > window->capacity) {
        *bytes = 0;
        return nullptr;
    }
    unsigned char *const region = window->pool + window->used;
    window->used += chunk;
    *bytes = chunk;
    return region;
}

/* A freed slot is handed out again before the cursor moves, so a process that
 * maps and unmaps reuses its CSpace instead of filling it (specs/userland.md).
 * This is the free-list half of the release. */
bool freed_slot_is_reused_holds()
{
    host_sel4::reset();
    host_sel4::set_shape(4, 12);
    g_chunks = 0;

    aegir::mem::Allocator allocator(nullptr);
    seL4_CPtr const root = host_sel4::make_root_untyped(22);
    if (!allocator.adopt_untyped(root, 22, 0)) {
        return false;
    }
    allocator.adopt_slots_level_two(20, (seL4_Word{1} << 12) - 20, 4, 12, 0, true);

    alignas(64) static unsigned char pool[1 << 16];
    static NodeWindow window;
    window = NodeWindow{&allocator, pool, sizeof(pool), 0};
    allocator.set_node_source(plain_nodes, &window);

    seL4_CPtr const a = allocator.alloc_slot();
    seL4_CPtr const b = allocator.alloc_slot();
    if (a == 0 || b == 0 || a == b) {
        return false;
    }
    allocator.free_slot(a);
    seL4_CPtr const c = allocator.alloc_slot();
    /* The cursor had not moved back, so a free list is the only thing that can
     * return `a`. */
    return c == a;
}

/* A child's spawn (specs/authority.md; aegir-spawn's process.cc and
 * child_vspace.cc): the image is one run of consecutive frames taken from a
 * single piece, and the child's own objects -- a root CNode, an L2 CNode, a TCB,
 * a notification, an endpoint -- are distinct capabilities in the same CSpace.
 * The run's frames must be consecutive and go back whole, and no object may land
 * on the run's slots. This is the use every process makes, and it is the run
 * path the suite did not touch. */
bool child_spawn_holds()
{
    host_sel4::reset();
    host_sel4::set_shape(4, 12);
    g_chunks = 0;
    g_chunk_bits = 22;

    aegir::mem::Allocator allocator(nullptr);
    seL4_CPtr const seed = host_sel4::make_root_untyped(26);
    if (!allocator.adopt_untyped(seed, 26, 0)) {
        return false;
    }
    allocator.set_untyped_source(source, nullptr);
    allocator.adopt_slots_level_two(20, (seL4_Word{1} << 12) - 20, 4, 12, 0, true);
    alignas(64) static unsigned char pool[1 << 16];
    static NodeWindow window;
    window = NodeWindow{&allocator, pool, sizeof(pool), 0};
    allocator.set_node_source(plain_nodes, &window);

    unsigned const before_collisions = host_sel4::collisions();
    seL4_Error error = seL4_NoError;
    void *cookie = nullptr;

    seL4_CPtr objs[5];
    objs[0] = allocator.alloc_object(seL4_CapTableObject, 8, g_account, &error, &cookie);
    objs[1] = allocator.alloc_object(seL4_CapTableObject, 12, g_account, &error, &cookie);
    objs[2] = allocator.alloc_object(seL4_TCBObject, 10, g_account, &error, &cookie);
    objs[3] = allocator.alloc_object(seL4_NotificationObject, 5, g_account, &error, &cookie);
    objs[4] = allocator.alloc_object(seL4_EndpointObject, 4, g_account, &error, &cookie);
    for (seL4_CPtr const cap : objs) {
        if (cap == 0 || !host_sel4::valid_cap(cap)) {
            return false;
        }
    }
    for (unsigned i = 0; i < 5; ++i) {
        for (unsigned j = i + 1; j < 5; ++j) {
            if (objs[i] == objs[j]) {
                return false; /* one object overwrote another */
            }
        }
    }

    void *run_cookie = nullptr;
    seL4_CPtr const first = allocator.alloc_pages_run(8, g_account, &error, &run_cookie);
    if (first == 0) {
        return false;
    }
    /* The image's frames are consecutive capabilities... */
    for (seL4_Word i = 0; i < 8; ++i) {
        if (!host_sel4::valid_cap(first + i)) {
            return false;
        }
    }
    /* ...and the run is one piece, so giving it back returns it whole. */
    if (!allocator.free_object(run_cookie, seL4_PageBits + 3)) {
        return false;
    }
    return host_sel4::collisions() == before_collisions;
}

/* A session's range is reclaimed (auth's `slot_release`): a mark is taken before
 * a session's allocations, and once the session's capabilities have been revoked
 * -- the kernel emptied the slots -- the cursor returns to the mark, so the next
 * session reuses the range instead of growing past it. This is the "a revoked
 * range comes back at once" half of freeing (specs/userland.md). */
bool session_reclaim_holds()
{
    host_sel4::reset();
    host_sel4::set_shape(0, 12);
    g_chunks = 0;

    aegir::mem::Allocator allocator(nullptr);
    seL4_CPtr const seed = host_sel4::make_root_untyped(20);
    if (!allocator.adopt_untyped(seed, 20, 0)) {
        return false;
    }
    allocator.adopt_slots(20, 400, 0, 12);

    seL4_CPtr const mark = allocator.slot_mark();
    seL4_CPtr held[16] = {};
    for (seL4_CPtr &slot : held) {
        slot = allocator.alloc_slot();
        if (slot == 0) {
            return false;
        }
    }
    /* The session is torn down: its caps are revoked (the kernel empties the
     * slots) and auth hands the range back. */
    allocator.slot_release(mark);

    for (seL4_CPtr const was : held) {
        if (allocator.alloc_slot() != was) {
            return false; /* the range did not come back */
        }
    }
    return true;
}

/* One CNode, two allocators (aegir-net): the process's own draws upward from the
 * first free slot and lwIP's draws downward from the end, over disjoint halves,
 * so neither can hand out a slot the other holds while anything is left. */
bool shared_cnode_holds()
{
    host_sel4::reset();
    host_sel4::set_shape(0, 12);

    aegir::mem::Allocator up(nullptr);
    aegir::mem::Allocator down(nullptr);
    constexpr seL4_CPtr kUpFirst = 20;
    constexpr seL4_Word kEach = 200;
    up.adopt_slots(kUpFirst, kEach, 0, 12);
    down.adopt_slots_down(kUpFirst + kEach, kEach, 0, 12);

    for (seL4_Word i = 0; i < kEach; ++i) {
        seL4_CPtr const a = up.alloc_slot();
        seL4_CPtr const b = down.alloc_slot();
        if (a == 0 || b == 0 || a == b) {
            return false;
        }
        /* Each stays in its own half. */
        if (a < kUpFirst || a >= kUpFirst + kEach) {
            return false;
        }
        if (b < kUpFirst + kEach || b >= kUpFirst + 2 * kEach) {
            return false;
        }
    }
    /* Only genuinely full are they out. */
    return up.alloc_slot() == 0 && down.alloc_slot() == 0;
}

/* A driver's device registers (aegir-director, aegir-partmgr's
 * `device_window`): `pages` consecutive frames at a physical base, which can
 * come from nowhere but a device untyped that covers it. A base the untyped does
 * not cover is refused rather than silently returning someone else's page. */
bool device_window_holds()
{
    host_sel4::reset();
    host_sel4::set_shape(0, 12);

    seL4_BootInfo info{};
    info.empty = {1, 8000};
    info.initThreadCNodeSizeBits = 12;
    constexpr uint64_t kBase = 0x10000000ull;
    seL4_CPtr const device = host_sel4::make_root_untyped(24);
    info.untypedList[0].sizeBits = 24;
    info.untypedList[0].isDevice = 1;
    info.untypedList[0].paddr = kBase;
    info.untyped = {device, device + 1};

    aegir::mem::Allocator allocator(&info);
    if (!allocator.initialise()) {
        return false;
    }

    seL4_Error error = seL4_NoError;
    seL4_CPtr first = 0;
    if (!allocator.device_window(kBase, 4, &first, &error)) {
        return false;
    }
    for (seL4_Word i = 0; i < 4; ++i) {
        if (!host_sel4::valid_cap(first + i)) {
            return false;
        }
    }
    /* A window outside the device untyped is refused, not answered with some
     * other frame. */
    seL4_CPtr other = 0;
    if (allocator.device_window(kBase + (1ull << 24), 1, &other, &error)) {
        return false;
    }
    return true;
}

/* A refused allocation gives back what it took (aegir-mem's `slot_failed` and
 * `slot_failed_run`): a retype that fails after the slot is reserved must return
 * the slot and the piece, or the CSpace loses them and a later allocation runs
 * out of a resource that is free. A caller sees zero; the allocator must still
 * be whole. */
bool failed_alloc_returns_what_it_took_holds()
{
    host_sel4::reset();
    host_sel4::set_shape(0, 12);
    g_chunks = 0;

    /* One piece of exactly the size the use needs, and *no* untyped source, so
     * a refused retype that drops its piece leaves the retry with nothing (a
     * larger piece could be split into one, a source could replace it, and
     * either would hide the leak). */
    alignas(64) static unsigned char object_pool[1 << 16];
    alignas(64) static unsigned char run_pool[1 << 16];

    /* A refused object must give its piece back, or the next attempt cannot
     * run. */
    {
        aegir::mem::Allocator allocator(nullptr);
        seL4_CPtr const piece = host_sel4::make_root_untyped(13);
        if (!allocator.adopt_untyped(piece, 13, 0)) {
            return false;
        }
        allocator.adopt_slots(20, 100, 0, 12);
        static NodeWindow window;
        window = NodeWindow{&allocator, object_pool, sizeof(object_pool), 0};
        allocator.set_node_source(plain_nodes, &window);

        seL4_Error error = seL4_NoError;
        void *cookie = nullptr;
        host_sel4::fail_next_retype_of(seL4_CapTableObject);
        if (allocator.alloc_object(seL4_CapTableObject, 8, g_account, &error, &cookie) != 0) {
            std::printf("  failed_alloc: the refused object was not refused\n");
            return false;
        }
        if (allocator.alloc_object(seL4_CapTableObject, 8, g_account, &error, &cookie) == 0) {
            std::printf("  failed_alloc: the refused object's piece was not returned\n");
            return false;
        }
        if (allocator.check_free_lists() != 0) {
            std::printf("  failed_alloc: object free lists inconsistent\n");
            return false;
        }
    }

    /* The same for a run: its piece must come back whole, or the retry has no
     * piece to retype from. A fresh model, because the two sub-tests share the
     * stub's cap space and would otherwise collide on the first slot. */
    {
        host_sel4::reset();
        aegir::mem::Allocator allocator(nullptr);
        seL4_CPtr const piece = host_sel4::make_root_untyped(15);
        if (!allocator.adopt_untyped(piece, 15, 0)) {
            return false;
        }
        allocator.adopt_slots(20, 100, 0, 12);
        static NodeWindow window;
        window = NodeWindow{&allocator, run_pool, sizeof(run_pool), 0};
        allocator.set_node_source(plain_nodes, &window);

        seL4_Error error = seL4_NoError;
        void *run_cookie = nullptr;
        host_sel4::fail_next_retype_of(seL4_RISCV_4K_Page);
        if (allocator.alloc_pages_run(8, g_account, &error, &run_cookie) != 0) {
            std::printf("  failed_alloc: the refused run was not refused\n");
            return false;
        }
        if (allocator.alloc_pages_run(8, g_account, &error, &run_cookie) == 0) {
            std::printf("  failed_alloc: the refused run's piece was not returned\n");
            return false;
        }
        if (allocator.check_free_lists() != 0) {
            std::printf("  failed_alloc: run free lists inconsistent\n");
            return false;
        }
    }
    return host_sel4::collisions() == 0;
}

/* A service's frame buffer (datatypes, files, network, font and socket all call
 * `alloc_page`): one frame, addressable, and two are distinct. */
bool frame_per_use_holds()
{
    host_sel4::reset();
    host_sel4::set_shape(4, 12);
    g_chunks = 0;
    g_chunk_bits = 21;

    aegir::mem::Allocator allocator(nullptr);
    seL4_CPtr const seed = host_sel4::make_root_untyped(23);
    if (!allocator.adopt_untyped(seed, 23, 0)) {
        return false;
    }
    allocator.set_untyped_source(source, nullptr);
    allocator.adopt_slots_level_two(20, (seL4_Word{1} << 12) - 20, 4, 12, 0, true);

    unsigned const before_collisions = host_sel4::collisions();
    seL4_Error error = seL4_NoError;
    seL4_CPtr const a = allocator.alloc_page(g_account, &error, seL4_PageBits);
    seL4_CPtr const b = allocator.alloc_page(g_account, &error, seL4_PageBits);
    return a != 0 && b != 0 && a != b && host_sel4::valid_cap(a) &&
           host_sel4::valid_cap(b) && host_sel4::collisions() == before_collisions;
}

/* A child's ASID pool (aegir-spawn's `make_asid_pool`): a pool object, from its
 * own untyped, named by a slot the caller can hand to the child. */
bool asid_pool_holds()
{
    host_sel4::reset();
    host_sel4::set_shape(0, 12);
    g_chunks = 0;

    aegir::mem::Allocator allocator(nullptr);
    seL4_CPtr const seed = host_sel4::make_root_untyped(20);
    if (!allocator.adopt_untyped(seed, 20, 0)) {
        return false;
    }
    allocator.adopt_slots(20, 100, 0, 12);

    seL4_Error error = seL4_NoError;
    seL4_CPtr const pool = allocator.make_asid_pool(g_account, &error);
    return pool != 0 && host_sel4::valid_cap(pool);
}

/* The allocator's diagnostics (`untyped_free`, `piece_state`) are a contract too:
 * tooling and refusals read them, so what they report must match the piece the
 * caller holds. */
bool diagnostics_contract_holds()
{
    host_sel4::reset();
    host_sel4::set_shape(0, 12);
    g_chunks = 0;

    aegir::mem::Allocator allocator(nullptr);
    seL4_CPtr const seed = host_sel4::make_root_untyped(20);
    if (!allocator.adopt_untyped(seed, 20, 0)) {
        return false;
    }
    allocator.set_untyped_source(source, nullptr);
    allocator.adopt_slots(20, 100, 0, 12);

    unsigned const free_before = allocator.untyped_free();
    seL4_Error error = seL4_NoError;
    void *cookie = nullptr;
    seL4_CPtr const cap =
        allocator.alloc_object(seL4_RISCV_4K_Page, seL4_PageBits, g_account, &error, &cookie);
    if (cap == 0) {
        return false;
    }
    aegir::mem::Allocator::Piece const piece = allocator.piece_state(cookie);
    /* The piece the caller holds is exactly a page wide, and it is not free. */
    return piece.size_bits == seL4_PageBits && !piece.free &&
           allocator.untyped_free() >= free_before;
}

}  // namespace

int main()
{
    host_sel4::reset();

    /* The node source allocates from the allocator it serves (the launcher's
     * `grow_nodes_from_window` does exactly this), so a refill can re-enter
     * itself. No piece may be lost to that. */
    expect(reentrancy_holds(), "a re-entrant refill keeps the allocator whole");

    /* The same, in the shape the console's slice takes: a big carved untyped
     * whose frames are retyped from the cap. This is where a spent piece handed
     * out by `carve_untyped` showed up on the target. */
    expect(carve_and_frames_hold(), "a carved slice's frames retype from their own cap");

    /* A caller that hands a piece back with its objects still alive (skipping
     * the revoke the contract asks for) must not poison the pool. */
    expect(free_without_revoke_holds(), "a piece freed without its objects revoked leaves no orphan");

    /* A service asks for frames, not for an untyped it must retype: the region
     * carves, retypes and gives the piece back whole (specs/memory.md). */
    expect(frame_region_holds(), "a frame region gives frames and the piece back whole");

    /* The CSpace half: a two-level process grows under slot pressure without a
     * caller-supplied reserve. This is the class the acceptance kept finding. */
    expect(two_level_growth_holds(), "a two-level process grows its CSpace under slot pressure");

    /* A split deeper than the CSpace's remaining slots must grow, not wedge. */
    expect(split_under_slot_pressure_holds(),
           "a deep split triggers growth instead of failing for slots");

    expect(freed_slot_is_reused_holds(), "a freed slot is handed out again before the cursor moves");

    /* The use every process makes: an image run and the child's own objects. */
    expect(child_spawn_holds(), "a child's image run and its objects are distinct and the run goes back whole");

    /* A session's range comes back on reclaim, and one CNode serves two owners. */
    expect(session_reclaim_holds(), "a reclaimed session range is handed out again");
    expect(shared_cnode_holds(), "two owners of one CNode never share a slot");

    /* A driver's registers are consecutive frames from the device's untyped. */
    expect(device_window_holds(), "a device's register window is consecutive frames at its base");

    expect(failed_alloc_returns_what_it_took_holds(),
           "a refused retype gives its slot and piece back");

    expect(frame_per_use_holds(), "a service's frame buffer is one addressable frame");
    expect(asid_pool_holds(), "a child's ASID pool is named by a slot");
    expect(diagnostics_contract_holds(), "the allocator's diagnostics describe the piece held");

    host_sel4::reset();

    /* The bootinfo: one 22-bit root untyped -- the launcher's seed -- and a run
     * of free slots. Growth comes from the source above. */
    seL4_BootInfo info{};
    info.empty = {1, 8000};
    info.initThreadCNodeSizeBits = 12;
    constexpr unsigned kRoots = 1;
    seL4_CPtr first = 0;
    for (unsigned i = 0; i < kRoots; ++i) {
        seL4_CPtr const cap = host_sel4::make_root_untyped(22);
        if (i == 0) {
            first = cap;
        }
        info.untypedList[i].sizeBits = 22;
        info.untypedList[i].isDevice = 0;
        /* Distinct physical addresses, so the allocator's address ordering is
         * exercised: a piece is spliced in among others rather than pushed on a
         * stack. */
        info.untypedList[i].paddr = 0x10000000ull + static_cast<uint64_t>(i) * 0x400000ull;
    }
    info.untyped = {first, first + kRoots};

    aegir::mem::Allocator allocator(&info);
    expect(allocator.initialise(), "the bootinfo is adopted");
    allocator.set_untyped_source(source, nullptr);
    allocator.set_trace(ring_trace, nullptr);

    std::vector<Live> live;
    constexpr unsigned kOps = 2000000;
    unsigned allocs = 0;
    unsigned frees = 0;
    unsigned first_broken = kOps;

    for (unsigned op = 0; op < kOps; ++op) {
        unsigned const before = host_sel4::refusals();
        unsigned const before_spent = host_sel4::spent();
        unsigned const before_collisions = host_sel4::collisions();
        unsigned const before_invalid = host_sel4::invalid();
        unsigned const roll = static_cast<unsigned>(rng() % 100);
        if (roll < 55 || live.empty()) {
            Shape const &shape = kShapes[rng() % (sizeof(kShapes) / sizeof(kShapes[0]))];
            seL4_Error error = seL4_NoError;
            void *cookie = nullptr;
            seL4_CPtr const cap =
                allocator.alloc_object(shape.type, shape.size_bits, g_account, &error, &cookie);
            if (cap != 0) {
                live.push_back(Live{cap, cookie,
                                    aegir::mem::Allocator::object_bits(shape.type,
                                                                       shape.size_bits)});
                ++allocs;
            }
        } else if (roll < 90) {
            std::size_t const index = rng() % live.size();
            auto const entry = live[index];
            /* A caller revokes the objects it retyped from a piece before
             * handing the piece back -- aegir-console does exactly this -- so
             * the piece is childless when the allocator deletes it. A carved
             * untyped is the piece itself, and its cap is the allocator's: the
             * caller revokes it but must not delete it (aegir-console's reap). */
            if (entry.carve) {
                (void)seL4_CNode_Revoke(seL4_CapInitThreadCNode, entry.cap, 12);
            } else {
                (void)seL4_CNode_Delete(seL4_CapInitThreadCNode, entry.cap, 12);
            }
            (void)allocator.free_object(entry.cookie, entry.wanted);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
            ++frees;
        } else if (roll < 95) {
            seL4_Word const bits = 12 + (rng() % 7);
            seL4_Error error = seL4_NoError;
            void *cookie = nullptr;
            seL4_CPtr const cap = allocator.carve_untyped(bits, g_account, &error, nullptr, &cookie);
            if (cap != 0) {
                live.push_back(Live{cap, cookie, bits, true});
                ++allocs;
            }
        } else {
            /* A spawn's staging is forgotten between commands (ServiceKit),
             * and the pieces it held go with it. The launcher's command
             * allocator draws slots from a pool a reset leaves alone; this one
             * has a cursor, so it is re-adopted. */
            allocator.reset();
            host_sel4::release_all();
            g_chunks = 0;
            allocator.adopt_slots(1, 7999, 12, 12);
            live.clear();
        }
        if (host_sel4::refusals() != before || host_sel4::spent() != before_spent ||
            host_sel4::collisions() != before_collisions || host_sel4::invalid() != before_invalid) {
            std::printf("a retype was refused at op %u (roll %u, %zu live): refusals %u spent %u "
                        "collisions %u invalid %u\n",
                        op, roll, live.size(), host_sel4::refusals() - before,
                        host_sel4::spent() - before_spent,
                        host_sel4::collisions() - before_collisions,
                        host_sel4::invalid() - before_invalid);
            dump_ring();
            break;
        }
        if ((op % 500u) == 0u) {
            unsigned const broken = allocator.check_free_lists();
            if (broken != 0 && first_broken == kOps) {
                first_broken = op;
                std::printf("free lists inconsistent at op %u (%u lists)\n", op, broken);
            }
        }
    }

    expect(first_broken == kOps, "the allocator's free lists stayed consistent");

    std::printf("allocator: %u ops, %u allocations, %u frees, %u retypes, %u refused\n", kOps,
                allocs, frees, host_sel4::retypes(), host_sel4::refusals());
    expect(host_sel4::refusals() == 0, "no retype ever ran out of a whole untyped");
    expect(host_sel4::spent() == 0, "no free-list piece was ever spent");
    expect(host_sel4::collisions() == 0, "no slot was ever handed out twice");
    expect(host_sel4::invalid() == 0, "no piece kept a cap the kernel no longer holds");
    std::printf("allocator: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
