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

typedef uint64_t seL4_Word;
typedef seL4_Word seL4_CPtr;
typedef seL4_Word seL4_Error;

/* The constants the allocator names. Values are the architecture's only where
 * the allocator compares them; the model uses the names. */
enum : seL4_Word {
    seL4_NoError = 0,
    seL4_NotEnoughMemory = 8,
    seL4_InvalidArgument = 1,
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

inline std::array<Cap, kMaxCaps> g_caps{};
/* Root untypeds the model hands out (a source's chunks) live well above the
 * slots a workload allocates, so a retyped child -- which lands in the slot the
 * allocator named -- never collides with one. */
inline seL4_CPtr g_next_root = 100000;
inline unsigned g_refusals = 0;
inline unsigned g_retypes = 0;
/* Caps deleted while capabilities derived from them were still alive: the
 * memory is orphaned (a delete does not take its children), which is what a
 * piece handed back without its objects revoked leaves behind. */
inline unsigned g_orphans = 0;
inline bool g_log = false;

inline seL4_CPtr take_root()
{
    return g_next_root++;
}

inline void reset()
{
    g_caps.fill(Cap{});
    g_next_root = 100000;
    g_refusals = 0;
    g_retypes = 0;
    g_orphans = 0;
    g_log = false;
}

/** The workload forgot its pieces (the allocator's reset): nothing the model
 *  remembers is referenced any more, so every cap is free to reuse. */
inline void release_all()
{
    g_caps.fill(Cap{});
    g_next_root = 100000;
}

/** A root untyped the test (or a source) hands the allocator. */
inline seL4_CPtr make_root_untyped(unsigned bits)
{
    seL4_CPtr const cap = take_root();
    g_caps[cap] = Cap{true, true, bits, 0, 0, 0};
    return cap;
}

inline unsigned refusals() { return g_refusals; }
inline unsigned retypes() { return g_retypes; }
inline unsigned orphans() { return g_orphans; }
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
                                  seL4_CPtr slot)
{
    ++g_retypes;
    if (service >= kMaxCaps || !g_caps[service].valid || !g_caps[service].untyped) {
        return seL4_InvalidArgument;
    }
    Cap &parent = g_caps[service];
    uint32_t const free_index = parent.children == 0 ? 0 : parent.free_index;
    uint64_t const free_bytes =
        (uint64_t{1} << parent.block_bits) - uint64_t{free_index} * 16;
    unsigned const bits = object_bits_for(type, size_bits);
    if (bits >= 64 || (free_bytes >> bits) < 1) {
        ++g_refusals;
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
    seL4_CPtr const child = slot;
    g_caps[child] = Cap{true, type == seL4_UntypedObject, static_cast<uint32_t>(size_bits),
                        0, 0, service};
    return seL4_NoError;
}

inline seL4_Error cnode_delete(seL4_CPtr node, seL4_CPtr cap)
{
    static_cast<void>(node);
    if (cap >= kMaxCaps || !g_caps[cap].valid) {
        return seL4_NoError;
    }
    if (g_caps[cap].children > 0) {
        ++g_orphans;
    }
    seL4_CPtr const parent = g_caps[cap].parent;
    if (parent != 0 && parent < kMaxCaps && g_caps[parent].valid &&
        g_caps[parent].children > 0) {
        g_caps[parent].children -= 1;
    }
    g_caps[cap].valid = false;
    return seL4_NoError;
}

/* A revoke takes every capability derived from `cap` with it -- unlike a delete,
 * which leaves them and so leaves the memory in use (kernel/src/kernel/
 * cspace.c `finaliseCap`, `capHasDerived`). The allocator's merge relies on it:
 * a piece can only be whole again when its cap has no children. */
inline seL4_Error cnode_revoke(seL4_CPtr node, seL4_CPtr cap)
{
    static_cast<void>(node);
    if (cap >= kMaxCaps || !g_caps[cap].valid) {
        return seL4_NoError;
    }
    for (seL4_CPtr child = 0; child < kMaxCaps; ++child) {
        if (g_caps[child].valid && g_caps[child].parent == cap) {
            (void)cnode_revoke(node, child);
            g_caps[child].valid = false;
        }
    }
    g_caps[cap].children = 0;
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
    static_cast<void>(node_index);
    static_cast<void>(node_depth);
    static_cast<void>(num_objects);
    return host_sel4::untyped_retype(service, type, size_bits, node_offset);
}

inline seL4_Error seL4_CNode_Delete(seL4_CPtr node, seL4_CPtr cap, seL4_Word depth)
{
    static_cast<void>(depth);
    return host_sel4::cnode_delete(node, cap);
}

inline seL4_Error seL4_CNode_Revoke(seL4_CPtr node, seL4_CPtr cap, seL4_Word depth)
{
    static_cast<void>(depth);
    return host_sel4::cnode_revoke(node, cap);
}

inline seL4_Error seL4_RISCV_ASIDControl_MakePool(seL4_CPtr control, seL4_CPtr untyped,
                                                  seL4_CPtr node_index, seL4_CPtr slot,
                                                  seL4_Word depth)
{
    static_cast<void>(control);
    static_cast<void>(untyped);
    static_cast<void>(node_index);
    static_cast<void>(slot);
    static_cast<void>(depth);
    return seL4_NoError;
}

#endif  // AEGIR_HOST_SEL4_STUB_H
