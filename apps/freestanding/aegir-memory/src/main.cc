/*
 * The memory service: one pool, chunks on demand, reclaimed by badge
 * (specs/memory.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Director delegates this service a large untyped -- most of the machine. It
 * splits it into fixed-size chunks once and serves them from a free list:
 * `alloc` hands a pristine chunk to a caller, owned by the caller's badge, and
 * `release` takes every chunk a badge holds back in one call, revoking what the
 * caller built from it first. The caller's runtime adopts the chunk and grows
 * its heap there; the caller's ceiling is its quota, not a compile-time pool.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/memory.h>

#include <sel4/sel4.h>

namespace {

void write(char const *text) noexcept
{
    aegir::debug_write(text);
}

void write_word(uint64_t value) noexcept
{
    aegir::debug_write_unsigned(value);
}

/* The pool's allocator and its node region: it splits the delegated untyped
 * into chunks, so it needs the untyped tree. Static, like every service's,
 * because an allocator is larger than a stack. */
aegir::mem::Allocator g_pool(nullptr);
alignas(64) unsigned char g_nodes[64 * 1024];

/* The chunks, and whose each is: 0 is free. The count is the pool's, computed
 * at startup; the array is BSS sized well beyond any pool a boot hands over. */
constexpr uint32_t kMaxChunks = 2048;
seL4_CPtr g_chunk[kMaxChunks];
uint64_t g_chunk_owner[kMaxChunks];
uint32_t g_chunk_count = 0;

/* A free chunk's index, or kMaxChunks when none is free. */
uint32_t chunk_free() noexcept
{
    for (uint32_t i = 0; i < g_chunk_count; ++i) {
        if (g_chunk_owner[i] == 0) {
            return i;
        }
    }
    return kMaxChunks;
}

/* Every chunk `owner` holds: revoke it -- the caller's frames and tables
 * derived from it go too -- and mark it free. */
uint64_t release_owner(uint64_t owner) noexcept
{
    uint64_t released = 0;
    for (uint32_t i = 0; i < g_chunk_count; ++i) {
        if (g_chunk_owner[i] == owner) {
            seL4_CNode_Revoke(aegir::bootstrap::kSlotOwnCNode, g_chunk[i],
                              aegir::bootstrap::kCNodeBits);
            g_chunk_owner[i] = 0;
            ++released;
        }
    }
    return released;
}

void answer_alloc(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                  uint64_t badge) noexcept
{
    uint64_t const wanted_bits = count >= 1 ? words[0] : aegir::memory::kChunkBits;
    uint64_t answer[1] = {aegir::memory::kChunkBits};
    if (wanted_bits > aegir::memory::kChunkBits) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint32_t const index = chunk_free();
    if (index == kMaxChunks) {
        port.reply_words(nullptr, 0);
        return;
    }
    g_chunk_owner[index] = badge;
    port.reply_cap(answer, 1, g_chunk[index]);
}

void answer_release(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                    uint64_t badge) noexcept
{
    uint64_t const owner = count >= 1 && words[0] != 0 ? words[0] : badge;
    uint64_t const released = release_owner(owner);
    port.reply_words(&released, 1);
}

/* Prove a chunk is a usable untyped: retype one frame from it and delete it
 * again. A chunk that cannot hold a frame is not memory anyone can grow in. */
bool self_test() noexcept
{
    if (g_chunk_count == 0) {
        return false;
    }
    seL4_CPtr const slot = g_pool.alloc_slot();
    if (slot == 0) {
        return false;
    }
    seL4_Error const error = seL4_Untyped_Retype(
        g_chunk[0], seL4_RISCV_4K_Page, seL4_PageBits, aegir::bootstrap::kSlotOwnCNode,
        aegir::bootstrap::kSlotOwnCNode, 0, slot, 1);
    if (error != seL4_NoError) {
        return false;
    }
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, slot,
                      aegir::bootstrap::kCNodeBits);
    return true;
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Owner port = aegir::ipc::Owner::find(aegir::memory::kPortName,
                                                     aegir::memory::kPortNameLength);
    if (!port.valid()) {
        write("  memory: no mem.main port to own\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The pool: the untyped director delegated. */
    uint64_t pool_slot = 0;
    uint64_t pool_physical = 0;
    uint32_t pool_bits = 0;
    uint64_t pool_address = 0;
    if (!aegir::bootstrap::capability("untyped", 7, &pool_slot) ||
        !aegir::bootstrap::untyped(&pool_physical, &pool_bits, &pool_address) ||
        pool_bits <= aegir::memory::kChunkBits) {
        write("  memory: no pool to split\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The slots past the block's names are ours; the allocator addresses them
     * at depth zero, as a service does. */
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            if (entry.kind == aegir::bootstrap::EntryKind::Capability &&
                entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            }
        }
    }
    g_pool.adopt_nodes(g_nodes, sizeof(g_nodes));
    g_pool.adopt_slots(first_free, (1u << aegir::bootstrap::kCNodeBits) - first_free, 0);
    if (!g_pool.adopt_untyped(static_cast<seL4_CPtr>(pool_slot), pool_bits, pool_physical)) {
        write("  memory: the pool would not be adopted\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* Split the pool into chunks once. The count is the pool's; a pool larger
     * than the table is a change to the table, not a quiet truncation. */
    uint32_t const wanted = 1u << (pool_bits - aegir::memory::kChunkBits);
    uint32_t const slots_left =
        (1u << aegir::bootstrap::kCNodeBits) - static_cast<uint32_t>(first_free);
    g_chunk_count = wanted < kMaxChunks ? wanted : kMaxChunks;
    if (g_chunk_count > slots_left) {
        g_chunk_count = slots_left;
    }
    seL4_Error error = seL4_NoError;
    for (uint32_t i = 0; i < g_chunk_count; ++i) {
        aegir::mem::Account account{"memory", 0, 0, 0};
        seL4_CPtr const chunk = g_pool.carve_untyped(aegir::memory::kChunkBits, account,
                                                     &error);
        if (chunk == 0) {
            g_chunk_count = i;
            break;
        }
        g_chunk[i] = chunk;
        g_chunk_owner[i] = 0;
    }

    write("  memory: pool ready, ");
    write_word(g_chunk_count);
    write(" chunks of 2 MiB\n");
    write(self_test() ? "  memory: a chunk retyped a frame\n"
                      : "  memory: FAIL a chunk could not retype a frame\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        seL4_Word badge = 0;
        uint32_t const method =
            port.receive_words(words, aegir::ipc::kMaxWords, &count, &badge);
        switch (method) {
        case aegir::memory::kMethodAlloc:
            answer_alloc(port, words, count, badge);
            break;
        case aegir::memory::kMethodRelease:
            answer_release(port, words, count, badge);
            break;
        default:
            /* A method this version does not know is answered by saying
             * nothing (specs/services.md's versioning rule). */
            port.reply_words(nullptr, 0);
            break;
        }
    }
}
