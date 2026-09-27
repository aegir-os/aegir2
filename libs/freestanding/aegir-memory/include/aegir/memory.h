/*
 * The memory service's port (specs/memory.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * `mem.main` hands out pristine untyped chunks from one large pool, on demand,
 * owned by the caller's badge and reclaimed together when a caller releases
 * them. A process's runtime adopts a chunk and retypes its frames, page tables
 * and heap from it, so a chunk is the unit of growth -- not a frame.
 */

#ifndef AEGIR_MEMORY_H
#define AEGIR_MEMORY_H

#include <stdint.h>

namespace aegir::memory {

constexpr char kPortName[] = "mem.main";
constexpr uint32_t kPortNameLength = 8;

/** alloc: in the size the caller wants in bits; answer the chunk's size in
 *  bits and one capability, a pristine untyped of that size. The chunk is
 *  owned by the calling capability's badge. A request larger than the biggest
 *  chunk is answered with nothing. */
constexpr uint32_t kMethodAlloc = 1;

/** release: in a badge (0 is the caller's own); answer how many chunks came
 *  back. Every chunk that badge owns is revoked -- the caller's objects derived
 *  from it go with it -- and freed for another caller. A spawner passes a
 *  child's badge here when the child exits. */
constexpr uint32_t kMethodRelease = 2;

/** The one chunk size class for now: 2 MiB, a large page, the granularity a
 *  program's runtime grows in (specs/memory.md). */
constexpr uint32_t kChunkBits = 21;
constexpr uint64_t kChunkBytes = 1ull << kChunkBits;

}  // namespace aegir::memory

#endif  // AEGIR_MEMORY_H
