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
#include <aegir/mem/slot_pool.h>

#include <cstdio>
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
};

/* The untyped source: a memory service handing out 21-bit chunks, bounded the
 * way the service's own pool is -- past its ceiling it answers nothing and an
 * allocation fails, rather than growing forever. */
constexpr unsigned kChunks = 64;
unsigned g_chunks = 0;

seL4_CPtr source(void *, seL4_Word *bits, uint64_t *paddr)
{
    if (g_chunks >= kChunks) {
        return 0;
    }
    /* A grown chunk arrives like the memory service's: a size and no address
     * (`command_untyped_source` answers zero), so the allocator has no order to
     * keep for it. Only the seed the process was started with has one. */
    *paddr = 0;
    ++g_chunks;
    *bits = 21;
    return host_sel4::make_root_untyped(21);
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

}  // namespace

int main()
{
    host_sel4::reset();

    /* The node source allocates from the allocator it serves (the launcher's
     * `grow_nodes_from_window` does exactly this), so a refill can re-enter
     * itself. No piece may be lost to that. */
    expect(reentrancy_holds(), "a re-entrant refill keeps the allocator whole");

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

    std::vector<Live> live;
    constexpr unsigned kOps = 2000000;
    unsigned allocs = 0;
    unsigned frees = 0;
    unsigned first_broken = kOps;

    for (unsigned op = 0; op < kOps; ++op) {
        unsigned const before = host_sel4::refusals();
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
             * the piece is childless when the allocator deletes it. */
            (void)seL4_CNode_Delete(seL4_CapInitThreadCNode, entry.cap, 12);
            (void)allocator.free_object(entry.cookie, entry.wanted);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
            ++frees;
        } else if (roll < 95) {
            seL4_Word const bits = 12 + (rng() % 7);
            seL4_Error error = seL4_NoError;
            void *cookie = nullptr;
            seL4_CPtr const cap = allocator.carve_untyped(bits, g_account, &error, nullptr, &cookie);
            if (cap != 0) {
                live.push_back(Live{cap, cookie, bits});
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
            allocator.adopt_slots(1, 7999, 12);
            live.clear();
        }
        if (host_sel4::refusals() != before) {
            std::printf("refused retype at op %u (roll %u, %zu live)\n", op, roll, live.size());
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
    expect(host_sel4::refusals() == 0, "no retype was ever made from a spent untyped");
    std::printf("allocator: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
