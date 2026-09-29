/*
 * Host conformance for the heap's free regions (specs/memory.md).
 *
 * The list is pure address arithmetic over caller-owned nodes: an
 * address-ordered walk, a first fit, a split, a neighbour merge and a node
 * pool that grows through a source. The boot proves the heap; this proves the
 * list, where a mistake would surface as a heap that corrupts itself under
 * pressure rather than as a line.
 */

#include "regions.h"

#include <cstdint>
#include <cstdio>

namespace {

using aegir::heap::detail::Regions;

constexpr uintptr_t kPage = 4096;
/* The regions are addresses only: the list never touches their contents (that
 * is the point of the nodes being elsewhere), so a synthetic span is enough. */
constexpr uintptr_t kBase = 0x10000000;

uintptr_t at(uintptr_t page)
{
    return kBase + page * kPage;
}

/* The node pool: a real, writable arena, adopted once. */
alignas(8) unsigned char g_nodes[32 * 1024];
/* A second arena the source hands over, once. */
alignas(8) unsigned char g_more_nodes[8 * 1024];
bool g_more_given = false;

void *one_more_region(void *context, unsigned *bytes)
{
    static_cast<void>(context);
    if (g_more_given) {
        return nullptr;
    }
    g_more_given = true;
    *bytes = sizeof(g_more_nodes);
    return g_more_nodes;
}

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

}  // namespace

int main()
{
    /* Nothing free is nothing to hand out. */
    {
        Regions regions;
        expect(regions.take(kPage) == nullptr, "an empty list takes nothing");
        expect(regions.empty(), "an empty list is empty");
    }

    /* A region given back comes back whole, at its own address. */
    {
        Regions regions;
        regions.adopt(g_nodes, sizeof(g_nodes));
        expect(regions.give(at(10), 4 * kPage), "a region is given back");
        expect(!regions.empty(), "a given region is not empty");
        expect(regions.take(4 * kPage) == reinterpret_cast<void *>(at(10)),
               "the whole region comes back at its base");
        expect(regions.empty(), "and the list is empty again");
    }

    /* A request smaller than the region splits it, handing out the top and
     * leaving the lower part free. */
    {
        Regions regions;
        regions.adopt(g_nodes, sizeof(g_nodes));
        expect(regions.give(at(10), 4 * kPage), "a region is given back");
        expect(regions.take(kPage) == reinterpret_cast<void *>(at(13)),
               "the top page comes out of a four-page region");
        expect(regions.bytes_free() == 3 * kPage, "three pages are left free");
        expect(regions.take(3 * kPage) == reinterpret_cast<void *>(at(10)),
               "the remainder comes back at the region's base");
        expect(regions.empty(), "and the list is empty again");
    }

    /* Adjacent regions merge, either order, so a request that only fits the
     * two together is answered. */
    {
        Regions regions;
        regions.adopt(g_nodes, sizeof(g_nodes));
        expect(regions.give(at(10), 2 * kPage), "the lower region is given");
        expect(regions.give(at(12), 2 * kPage), "the upper region is given");
        expect(regions.count() == 1, "the two merge into one");
        expect(regions.bytes_free() == 4 * kPage, "the merged region is four pages");
        expect(regions.take(4 * kPage) == reinterpret_cast<void *>(at(10)),
               "the whole merged region comes back");
    }
    {
        Regions regions;
        regions.adopt(g_nodes, sizeof(g_nodes));
        expect(regions.give(at(12), 2 * kPage), "the upper region is given first");
        expect(regions.give(at(10), 2 * kPage), "the lower region is given second");
        expect(regions.count() == 1, "the two merge backwards too");
        expect(regions.take(4 * kPage) == reinterpret_cast<void *>(at(10)),
               "the whole merged region comes back");
    }

    /* Three adjacent regions merge into one across two releases. */
    {
        Regions regions;
        regions.adopt(g_nodes, sizeof(g_nodes));
        expect(regions.give(at(10), kPage), "the first third is given");
        expect(regions.give(at(12), kPage), "the third third is given");
        expect(regions.give(at(11), kPage), "the middle joins them");
        expect(regions.count() == 1, "the three merge into one");
        expect(regions.take(3 * kPage) == reinterpret_cast<void *>(at(10)),
               "the three come back as one");
    }

    /* A first fit is the lowest region that holds the request, and a request
     * no region holds is refused without disturbing the list. */
    {
        Regions regions;
        regions.adopt(g_nodes, sizeof(g_nodes));
        expect(regions.give(at(10), 2 * kPage), "a lower region is given");
        expect(regions.give(at(14), 2 * kPage), "an upper region is given");
        expect(regions.take(2 * kPage) == reinterpret_cast<void *>(at(10)),
               "the lowest fitting region is taken");
        expect(regions.count() == 1, "the other region is still free");
        expect(regions.take(2 * kPage) == reinterpret_cast<void *>(at(14)),
               "and then the other is taken");
        expect(regions.take(kPage) == nullptr, "a request with nothing fitting is refused");
    }

    /* Regions that overlap or are malformed are refused, not merged into a list
     * that would hand out the same page twice. */
    {
        Regions regions;
        regions.adopt(g_nodes, sizeof(g_nodes));
        expect(regions.give(at(10), 4 * kPage), "a region is given");
        expect(!regions.give(at(12), kPage), "a region inside a free one is refused");
        expect(!regions.give(at(13), 2 * kPage), "a partially overlapping region is refused");
        expect(!regions.give(at(13) + 4, kPage), "a misaligned region is refused");
        expect(!regions.give(at(10), 0), "an empty region is refused");
        expect(regions.count() == 1, "the refusals left the list alone");
        expect(regions.bytes_free() == 4 * kPage, "and its bytes");
    }

    /* Turnover: give, take, give again, and the list neither grows nor loses
     * nodes -- every take frees what its give allocated. */
    {
        Regions regions;
        regions.adopt(g_nodes, sizeof(g_nodes));
        for (unsigned round = 0; round < 64; ++round) {
            expect(regions.give(at(10), 6 * kPage), "a region is given back");
            expect(regions.take(5 * kPage) == reinterpret_cast<void *>(at(11)),
                   "five of six pages come out");
            expect(regions.take(kPage) == reinterpret_cast<void *>(at(10)),
                   "and the last page");
            expect(regions.empty(), "the round ends empty");
        }
    }

    /* A pool that runs out refuses the release rather than corrupting the list;
     * with a source set, it does not run out. */
    {
        Regions regions;
        alignas(8) static unsigned char tiny[4 * sizeof(uintptr_t) * 4];
        regions.adopt(tiny, sizeof(tiny));
        unsigned given = 0;
        for (unsigned i = 0; i < 32; ++i) {
            if (regions.give(at(10 + 4 * i), 2 * kPage)) {
                ++given;
            }
        }
        expect(given > 0 && given < 32, "a small pool gives some and then refuses");
        expect(regions.count() == given, "the refusals added nothing");
        regions.set_node_source(one_more_region, nullptr);
        expect(regions.give(at(10 + 4 * 32), 2 * kPage), "the source lets it grow");
    }

    std::printf("regions: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
