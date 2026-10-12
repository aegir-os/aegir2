/*
 * A host stub of the seL4 surface aegir-mem's allocator uses.
 *
 * The allocator's bookkeeping -- splitting, the buddy merge, the free lists --
 * is what a host test needs to exercise, and none of it needs the kernel: it
 * needs the *rule* the kernel applies, so that a retype from a piece whose
 * untyped is already spent is refused the way the kernel refuses it. This
 * header keeps that rule (the free index, the childless reset, the alignment,
 * and `object_bits`) and nothing else, so `check_allocator.py` can replay
 * allocator traffic in seconds instead of a six-minute QEMU boot.
 *
 * It shadows <sel4/sel4.h> on the include path; it is never part of a target
 * build (the real header is).
 *
 * The model, from kernel/src/object/untyped.c `decodeUntypedInvocation`:
 *   - a cap's free index is where its own retypes stopped, in 16-byte units;
 *   - a cap with no children starts again at zero (the reset, :182-189);
 *   - `untypedFreeBytes = 2^block - free_index * 16`, and a retype of an object
 *     costing 2^object_bits is refused when `untypedFreeBytes >> object_bits` is
 *     zero -- the "0 bytes available" the launcher's spawn printed.
 */

#ifndef AEGIR_HOST_SEL4_STUB_H
#define AEGIR_HOST_SEL4_STUB_H

#include <stdint.h>

#include <array>
#include <cstdio>
#include <vector>

typedef uint64_t seL4_Word;
typedef seL4_Word seL4_CPtr;
typedef seL4_Word seL4_Error;

/* The kernel's is a macro (`sel4/types.h`), so the allocator's `remaining <
 * seL4_UntypedRetypeMaxObjects ? remaining : seL4_UntypedRetypeMaxObjects` mixes
 * a word and an integer, not a word and an enum. */
#define seL4_UntypedRetypeMaxObjects 256

/* The constants the allocator names. Values are the architecture's only where
 * the allocator compares them; the model uses the names. */
enum : seL4_Word {
    seL4_NoError = 0,
    seL4_InvalidArgument = 1,
    seL4_InvalidCapability = 2,
    seL4_FailedLookup = 6,
    seL4_DeleteFirst = 8,
    seL4_NotEnoughMemory = 10,
};
enum : unsigned {
    seL4_WordBits = 64,
    seL4_PageBits = 12,
    seL4_SlotBits = 5,
    seL4_UntypedObject = 0,
    seL4_CapTableObject = 1,
    seL4_TCBObject = 2,
    seL4_EndpointObject = 3,
    seL4_NotificationObject = 4,
    seL4_RISCV_4K_Page = 5,
    seL4_RISCV_Mega_Page = 6,
    seL4_RISCV_PageTableObject = 7,
    seL4_PageTableBits = 12,
    CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS = 230,
};
enum : seL4_CPtr {
    seL4_CapInitThreadCNode = 1,
    seL4_CapASIDControl = 2,
};

struct seL4_SlotRegion {
    seL4_Word start;
    seL4_Word end;
};

struct seL4_UntypedDesc {
    seL4_Word paddr;
    uint8_t sizeBits;
    uint8_t isDevice;
    uint8_t pad[6];
};

struct seL4_BootInfo {
    seL4_SlotRegion empty;
    seL4_SlotRegion untyped;
    seL4_Word initThreadCNodeSizeBits;
    seL4_UntypedDesc untypedList[CONFIG_MAX_NUM_BOOTINFO_UNTYPED_CAPS];
};

namespace host_sel4 {

/* One capability. An untyped tracks where its own retypes stopped and how many
 * caps were made from it; a cap with no children starts over. */
struct Cap {
    bool valid = false;
    bool untyped = false;
    uint32_t block_bits = 0;
    uint32_t free_index = 0;
    uint32_t children = 0;
    seL4_CPtr parent = 0;
};

constexpr unsigned kMaxCaps = 1u << 18;

/* The process's CSpace shape, set by a test before a two-level case. `l1 == 0`
 * is single-level, where a plain CPtr is the CNode itself and the index is the
 * flat cap number. A two-level process holds its objects in an L2 CNode at root
 * slot `cnode_index`, addressed `(cnode_index << l2) | offset`; the root CNode's
 * own slots (the L2 caps) are indexed above that range, so a root slot and an L2
 * offset with the same number cannot collide -- the collision the allocator's
 * `level_one_high_` exists to avoid, and which the flat stub could not express. */
inline unsigned g_l1 = 0;
inline unsigned g_l2 = 12;
inline void set_shape(unsigned l1, unsigned l2) { g_l1 = l1; g_l2 = l2; }
inline seL4_CPtr root_slot_base() { return static_cast<seL4_CPtr>(1) << (g_l1 + g_l2); }
inline seL4_CPtr resolve_cap(seL4_CPtr cap, seL4_Word depth)
{
    if (g_l1 == 0) {
        return cap;
    }
    if (depth == g_l1) {
        return root_slot_base() + cap;
    }
    return cap;
}

inline std::array<Cap, kMaxCaps> g_caps{};
/* The caps that have been created, so a revoke walks what exists (a few dozen)
 * rather than the whole 2^18-cap address space for every level of a merge. */
inline std::vector<seL4_CPtr> g_live;
/* Root untypeds the model hands out (a source's chunks) live well above the
 * slots a workload allocates, so a retyped child -- which lands in the slot the
 * allocator named -- never collides with one. */
inline seL4_CPtr g_next_root = 100000;
inline unsigned g_refusals = 0;
inline unsigned g_retypes = 0;
/* A refusal where the cap's free index was already past its start -- a retype
 * from an untyped that has been used since it was handed out. The allocator's
 * `piece_is_whole` probe provokes exactly this to detect a spent free-list
 * piece; a retype that is not a probe and hits it is the bug. */
inline unsigned g_spent = 0;
/* Caps deleted while capabilities derived from them were still alive: the
 * memory is orphaned (a delete does not take its children), which is what a
 * piece handed back without its objects revoked leaves behind. */
inline unsigned g_orphans = 0;
/* A retype that landed in a slot already holding a live cap: the allocator
 * handed the same slot out twice, so the model's cap no longer tracks the
 * allocator's piece. The kernel refuses this -- the destination slot is
 * occupied (`seL4_DeleteFirst`) -- so a correct allocator answers zero. */
inline unsigned g_collisions = 0;
/* A retype naming a cap the model no longer holds (invalid, or not an untyped):
 * the allocator kept a piece whose cap is gone. The kernel's answer is
 * `seL4_InvalidArgument`; a correct allocator answers zero. */
inline unsigned g_invalid = 0;
inline bool g_log = false;
/* A test that wants a retype of a given type to fail: the next one answers
 * NotEnoughMemory without consuming. Targeting the type matters -- a split also
 * retypes, of `seL4_UntypedObject`, so a bare flag fails the split and exercises
 * `refill`, not the caller's `slot_failed`. */
inline seL4_Word g_fail_retype_type = ~seL4_Word{0};
inline void fail_next_retype_of(seL4_Word type) { g_fail_retype_type = type; }

/** Whether the model holds a live cap at `cap` (an encoded L2 slot, or a kernel
 *  root cap the model handed out). Tests use it to check a cap an allocator
 *  returned is real. */
inline bool valid_cap(seL4_CPtr cap)
{
    return cap < kMaxCaps && g_caps[cap].valid;
}

inline seL4_CPtr take_root()
{
    return g_next_root++;
}

inline void reset()
{
    g_caps.fill(Cap{});
    g_live.clear();
    g_l1 = 0;
    g_l2 = 12;
    g_next_root = 100000;
    g_refusals = 0;
    g_retypes = 0;
    g_orphans = 0;
    g_spent = 0;
    g_collisions = 0;
    g_invalid = 0;
    g_log = false;
    g_fail_retype_type = ~seL4_Word{0};
}

/** The workload forgot its pieces (the allocator's reset): nothing the model
 *  remembers is referenced any more, so every cap is free to reuse. */
inline void release_all()
{
    g_caps.fill(Cap{});
    g_live.clear();
    g_next_root = 100000;
}

/** A root untyped the test (or a source) hands the allocator. */
inline seL4_CPtr make_root_untyped(unsigned bits)
{
    seL4_CPtr const cap = take_root();
    g_caps[cap] = Cap{true, true, bits, 0, 0, 0};
    g_live.push_back(cap);
    return cap;
}

inline unsigned refusals() { return g_refusals; }
inline unsigned retypes() { return g_retypes; }
inline unsigned orphans() { return g_orphans; }
inline unsigned spent() { return g_spent; }
inline unsigned collisions() { return g_collisions; }
inline unsigned invalid() { return g_invalid; }
inline void log_refusals(bool on) { g_log = on; }

/** The kernel's `getObjectSize`: a CNode's memory is its slot bits plus the
 *  bookkeeping slot (kernel/src/object/objecttype.c:42-48). */
inline unsigned object_bits_for(seL4_Word type, seL4_Word size_bits)
{
    if (type == seL4_CapTableObject) {
        return static_cast<unsigned>(size_bits) + seL4_SlotBits;
    }
    return static_cast<unsigned>(size_bits);
}

inline seL4_Error untyped_retype(seL4_CPtr service, seL4_Word type, seL4_Word size_bits,
                                  seL4_CPtr node_index, seL4_Word node_depth,
                                  seL4_CPtr node_offset)
{
    ++g_retypes;
    if (service >= kMaxCaps || !g_caps[service].valid || !g_caps[service].untyped) {
        ++g_invalid;
        if (g_invalid <= 8) {
            std::printf("  invalid retype: cap %llu valid=%d untyped=%d (type=%llu size=%llu)\n",
                        static_cast<unsigned long long>(service),
                        service < kMaxCaps ? g_caps[service].valid : -1,
                        service < kMaxCaps ? g_caps[service].untyped : -1,
                        static_cast<unsigned long long>(type),
                        static_cast<unsigned long long>(size_bits));
        }
        return seL4_InvalidArgument;
    }
    Cap &parent = g_caps[service];
    if (type == g_fail_retype_type) {
        g_fail_retype_type = ~seL4_Word{0};
        return seL4_NotEnoughMemory;
    }
    uint32_t const free_index = parent.children == 0 ? 0 : parent.free_index;
    uint64_t const free_bytes =
        (uint64_t{1} << parent.block_bits) - uint64_t{free_index} * 16;
    unsigned const bits = object_bits_for(type, size_bits);
    if (bits >= 64 || (free_bytes >> bits) < 1) {
        if (free_index != 0) {
            ++g_spent;
        } else {
            ++g_refusals;
        }
        if (g_log) {
            std::printf("  refused: cap=%llu type=%llu size=%llu block=%u free_index=%u "
                        "children=%u free_bytes=%llu\n",
                        static_cast<unsigned long long>(service),
                        static_cast<unsigned long long>(type),
                        static_cast<unsigned long long>(size_bits), parent.block_bits,
                        free_index, parent.children,
                        static_cast<unsigned long long>(free_bytes));
        }
        return seL4_NotEnoughMemory;
    }
    uint64_t const width = uint64_t{1} << bits;
    uint64_t const offset = (uint64_t{free_index} * 16 + width - 1) & ~(width - 1);
    parent.free_index = static_cast<uint32_t>((offset + width) / 16);
    parent.children += 1;
    /* The destination: the root CNode itself when single-level or when a CNode
     * is retyped into a root slot (`alloc_cnode_at_l1`, depth 0), and the L2
     * CNode at root slot `node_index` otherwise. */
    seL4_CPtr const child =
        g_l1 == 0 ? node_offset
                  : (node_depth == 0 ? root_slot_base() + node_offset
                                     : ((node_index << g_l2) | node_offset));
    /* A retype lands in a slot the allocator named; that slot must be free, or
     * the model's cap for it no longer tracks the allocator's piece. The kernel
     * refuses an occupied destination, so a collision here is the allocator
     * handing one slot out twice. */
    if (g_caps[child].valid) {
        ++g_collisions;
        if (g_collisions <= 4) {
            std::printf("  collision: slot %llu already holds cap(valid=%d untyped=%d block=%u "
                        "children=%u parent=%llu)\n",
                        static_cast<unsigned long long>(child), g_caps[child].valid,
                        g_caps[child].untyped, g_caps[child].block_bits, g_caps[child].children,
                        static_cast<unsigned long long>(g_caps[child].parent));
        }
    }
    g_caps[child] = Cap{true, type == seL4_UntypedObject, static_cast<uint32_t>(size_bits),
                        0, 0, service};
    g_live.push_back(child);
    return seL4_NoError;
}

inline seL4_Error cnode_delete(seL4_CPtr node, seL4_CPtr cap, seL4_Word depth)
{
    static_cast<void>(node);
    seL4_CPtr const index = resolve_cap(cap, depth);
    if (index >= kMaxCaps || !g_caps[index].valid) {
        return seL4_NoError;
    }
    if (g_caps[index].children > 0) {
        ++g_orphans;
    }
    seL4_CPtr const parent = g_caps[index].parent;
    if (parent != 0 && parent < kMaxCaps && g_caps[parent].valid &&
        g_caps[parent].children > 0) {
        g_caps[parent].children -= 1;
    }
    g_caps[index].valid = false;
    return seL4_NoError;
}

/* A revoke takes every capability derived from `cap` with it -- unlike a delete,
 * which leaves them and so leaves the memory in use (kernel/src/kernel/
 * cspace.c `finaliseCap`, `capHasDerived`). The allocator's merge relies on it:
 * a piece can only be whole again when its cap has no children. */
inline seL4_Error cnode_revoke(seL4_CPtr node, seL4_CPtr cap, seL4_Word depth)
{
    static_cast<void>(node);
    seL4_CPtr const index = resolve_cap(cap, depth);
    if (index >= kMaxCaps || !g_caps[index].valid) {
        return seL4_NoError;
    }
    /* Everything whose parent chain reaches `index`, deleted once each. Walking
     * the live set (not the whole cap space) keeps a merge's repeated revokes
     * cheap; the chain is at most a piece's split depth. */
    for (seL4_CPtr const child : g_live) {
        if (child >= kMaxCaps || !g_caps[child].valid || child == index) {
            continue;
        }
        seL4_CPtr at = g_caps[child].parent;
        bool derived = false;
        while (at != 0 && at < kMaxCaps && g_caps[at].valid) {
            if (at == index) {
                derived = true;
                break;
            }
            at = g_caps[at].parent;
        }
        if (derived) {
            g_caps[child].valid = false;
        }
    }
    g_caps[index].children = 0;
    return seL4_NoError;
}

}  // namespace host_sel4

/* The names the allocator calls, with the kernel's signatures (the arguments
 * the model does not need are ignored). */
inline seL4_Error seL4_Untyped_Retype(seL4_CPtr service, seL4_Word type,
                                      seL4_Word size_bits, seL4_CPtr root,
                                      seL4_CPtr node_index, seL4_Word node_depth,
                                      seL4_Word node_offset, seL4_Word num_objects)
{
    static_cast<void>(root);
    /* The kernel fills `num_objects` consecutive destination slots, consuming
     * that many objects from the untyped (kernel/src/object/untyped.c). A run is
     * exactly this, so the model must too, or `alloc_pages_run` looks like one
     * frame. */
    seL4_Error result = seL4_NoError;
    for (seL4_Word i = 0; i < num_objects; ++i) {
        result = host_sel4::untyped_retype(service, type, size_bits, node_index,
                                           node_depth, node_offset + i);
        if (result != seL4_NoError) {
            return result;
        }
    }
    return result;
}

inline seL4_Error seL4_CNode_Delete(seL4_CPtr node, seL4_CPtr cap, seL4_Word depth)
{
    return host_sel4::cnode_delete(node, cap, depth);
}

inline seL4_Error seL4_CNode_Revoke(seL4_CPtr node, seL4_CPtr cap, seL4_Word depth)
{
    return host_sel4::cnode_revoke(node, cap, depth);
}

inline seL4_Error seL4_RISCV_ASIDControl_MakePool(seL4_CPtr control, seL4_CPtr untyped,
                                                  seL4_CPtr node_index, seL4_CPtr slot,
                                                  seL4_Word depth)
{
    static_cast<void>(control);
    static_cast<void>(untyped);
    static_cast<void>(node_index);
    static_cast<void>(depth);
    /* The pool cap lands in `slot`, so a caller can name it. */
    if (slot < host_sel4::kMaxCaps) {
        host_sel4::g_caps[slot] = host_sel4::Cap{true, false, 0, 0, 0, 0};
        host_sel4::g_live.push_back(slot);
    }
    return seL4_NoError;
}

#endif  // AEGIR_HOST_SEL4_STUB_H
