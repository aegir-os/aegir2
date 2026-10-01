/*
 * The memory service: one pool, chunks on demand, reclaimed by badge, and the
 * resource limits an operator configured (specs/memory.md, specs/limits.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Director delegates this service the untypeds it did not otherwise spend --
 * the machine's memory, less the boot set -- so the pool is the host's size,
 * not a constant. It carves a chunk from the pool when a caller asks and gives
 * it back when the caller releases, so what the service's CSpace bounds is the
 * chunks *alive*, not the pool: more memory is more pool, and one untyped
 * carries it.
 *
 * At boot it reads the user database for each user's class and
 * Sys:S/limits.manifest for the rules, resolves each user's memory limits
 * once, and refuses or reports at alloc when a user badge crosses one. A
 * system badge is the superuser and is never limited (specs/authority.md).
 */

#include <aegir/authdb.h>
#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/limits.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/slot_pool.h>
#include <aegir/mem/vspace.h>
#include <aegir/memory.h>
#include <aegir/vfs.h>

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

/* A trace of the pool's pieces, for the "0 bytes available" hunt: only the
 * chunks' sizes (>= 20 bits), so a chunk that appears spent stands out. */
void memory_trace(void *context, char const *event, unsigned size_bits,
                  uint64_t physical, bool split_child, seL4_CPtr cap) noexcept
{
    static_cast<void>(context);
    static_cast<void>(physical);
    static_cast<void>(cap);
    if (size_bits < 20) {
        return;
    }
    write("  memory: mem ");
    write(event);
    write(" bits ");
    write_word(size_bits);
    write(split_child ? " split\n" : " root\n");
}

uint32_t field_length(char const *field, uint32_t bound) noexcept
{
    uint32_t length = 0;
    while (length < bound && field[length] != '\0') {
        ++length;
    }
    return length;
}

/* The pool's allocator, its node region, and the slot pool over the service's
 * own slots: it carves and frees chunks over its life, so slots must come
 * back. Static, like every service's, because an allocator is larger than a
 * stack. */
aegir::mem::Allocator g_pool(nullptr);
aegir::mem::Scratch g_scratch(nullptr);
aegir::mem::SlotPool g_slots;
constexpr uint32_t kPoolOwner = 1; /* the service's own slots, one owner */
constexpr uint32_t kMaxSlots = 4096;
uint32_t g_slot_owners[kMaxSlots];
alignas(64) unsigned char g_nodes[64 * 1024];

/* The chunks alive now: a cap the service keeps (to revoke), the allocator's
 * cookie for it (to free), how wide it is, and whose it is. Zero owner is
 * free. A chunk is whatever size its owner asked for (specs/memory.md), not a
 * fixed size, so the width has to travel with it. */
struct Chunk {
    seL4_CPtr cap;
    void *cookie;
    uint64_t owner;
    uint32_t bits;
};
constexpr uint32_t kMaxChunks = 2048;
Chunk g_chunk[kMaxChunks];

/* The pool's width in bits: the ceiling on a chunk's size. */
uint32_t g_pool_bits = 0;

/* The resolved limits (specs/limits.md): one row per user in the database,
 * indexed by the user index a badge carries, plus the committed bytes the user
 * holds. The rules' names and the file text live in the arena. */
struct UserRule {
    aegir::limits::Amount log;
    aegir::limits::Amount deny;
};
aegir::limits::Limits g_limits;
UserRule *g_rules = nullptr;
uint64_t *g_committed = nullptr;
uint32_t g_user_count = 0;

uint32_t chunk_free() noexcept
{
    for (uint32_t i = 0; i < kMaxChunks; ++i) {
        if (g_chunk[i].owner == 0 && g_chunk[i].cap == 0) {
            return i;
        }
    }
    return kMaxChunks;
}

/* Give one owner's chunk back to the pool and its committed bytes back to the
 * user, if it is a user's. */
void release_chunk(uint32_t index) noexcept
{
    uint64_t const owner = g_chunk[index].owner;
    uint32_t const bits = g_chunk[index].bits;
    seL4_CNode_Revoke(aegir::bootstrap::kSlotOwnCNode, g_chunk[index].cap,
                      aegir::bootstrap::kCNodeBits);
    g_chunk[index].cap = 0;
    (void)g_pool.free_object(g_chunk[index].cookie, bits);
    g_chunk[index].cookie = nullptr;
    g_chunk[index].owner = 0;
    g_chunk[index].bits = 0;
    if (aegir::ipc::is_user_badge(owner) && g_committed != nullptr) {
        uint64_t const user = aegir::ipc::user_index(owner);
        if (user < g_user_count) {
            uint64_t const bytes = 1ull << bits;
            g_committed[user] = g_committed[user] > bytes ? g_committed[user] - bytes : 0;
        }
    }
}

/* Every chunk `owner` holds: revoke it -- the caller's frames and tables
 * derived from it go too -- free it back to the pool, and mark it free. */
uint64_t release_owner(uint64_t owner) noexcept
{
    uint64_t released = 0;
    for (uint32_t i = 0; i < kMaxChunks; ++i) {
        if (g_chunk[i].cap != 0 && g_chunk[i].owner == owner) {
            release_chunk(i);
            ++released;
        }
    }
    return released;
}

/* ---- reading the configuration (specs/limits.md) ---- */

enum class ReadResult { Ok, NotReady, Failed };

/* Resolve `path` and read the whole file into arena memory. NotReady when the
 * volume is not registered yet -- a boot service racing the filesystem, which
 * asks again -- and Failed when it resolved but would not read. */
ReadResult read_file(aegir::vfs::Namespace &space, char const *path, uint32_t length,
                     seL4_CPtr slot, aegir::mem::Arena &arena, char **out,
                     uint32_t *out_length) noexcept
{
    aegir::vfs::Namespace::Resolved resolved{};
    if (!space.resolve(path, length, slot, resolved)) {
        return ReadResult::NotReady;
    }
    aegir::vfs::Volume volume(resolved.volume);
    aegir::vfs::Volume::Info info{};
    if (!volume.stat(resolved.rest, resolved.rest_length, info)) {
        /* The cap is minted now, and the retry resolves into the same slot: a
         * slot left occupied refuses the next mint, so drop it. The retry is
         * the volume not being ready, which is a boot race, not a fault. */
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, slot,
                          aegir::bootstrap::kCNodeBits);
        return ReadResult::NotReady;
    }
    if (info.size == 0) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, slot,
                          aegir::bootstrap::kCNodeBits);
        return ReadResult::Failed;
    }
    auto *buffer = static_cast<char *>(arena.allocate(info.size));
    if (buffer == nullptr) {
        return ReadResult::Failed;
    }
    uint64_t offset = 0;
    while (offset < info.size) {
        aegir::vfs::Volume::Bytes bytes{};
        if (!volume.read(resolved.rest, resolved.rest_length, offset, info.size - offset,
                         bytes) ||
            bytes.count == 0) {
            return ReadResult::Failed;
        }
        for (uint64_t i = 0; i < bytes.count; ++i) {
            buffer[offset + i] = bytes.data[i];
        }
        offset += bytes.count;
    }
    *out = buffer;
    *out_length = static_cast<uint32_t>(offset);
    return ReadResult::Ok;
}

/* Resolve the user database and the limits file, then resolve each user's
 * (memory, log) and (memory, deny) once by precedence. False leaves the
 * service unlimited, which is the default anyway. */
bool load_config(aegir::vfs::Namespace &space, aegir::mem::Arena &arena) noexcept
{
    seL4_CPtr const users_slot = g_pool.alloc_slot();
    seL4_CPtr const limits_slot = g_pool.alloc_slot();
    if (users_slot == 0 || limits_slot == 0) {
        return false;
    }

    /* Initrd: is up before this service starts (it is a boot service like this
     * one); the read still retries, because a boot is a race by nature. */
    char *users = nullptr;
    uint32_t users_length = 0;
    for (;;) {
        ReadResult const result = read_file(space, "Initrd:users.db", 15, users_slot, arena,
                                            &users, &users_length);
        if (result == ReadResult::Ok) {
            break;
        }
        if (result == ReadResult::Failed) {
            write("  memory: the user database would not read\n");
            return false;
        }
        seL4_Yield();
    }

    /* Sys: comes up after the VFS and after the filesystems behind it, so the
     * retry waits for the boot to bring it up -- auth's idiom for C:. */
    char *text = nullptr;
    uint32_t text_length = 0;
    for (;;) {
        ReadResult const result =
            read_file(space, "Sys:S/limits.manifest", 21, limits_slot, arena, &text,
                      &text_length);
        if (result == ReadResult::Ok) {
            break;
        }
        if (result == ReadResult::Failed) {
            write("  memory: Sys:S/limits.manifest would not read\n");
            return false;
        }
        seL4_Yield();
    }
    if (!g_limits.parse(text, text_length)) {
        write("  memory: the limits file is malformed at line ");
        write_word(g_limits.problem().line);
        write("\n");
        return false;
    }

    if (users_length < aegir::authdb::kHeaderBytes) {
        return false;
    }
    auto const *header = reinterpret_cast<uint32_t const *>(users);
    if (header[0] != aegir::authdb::kMagic || header[1] != aegir::authdb::kVersion) {
        write("  memory: the user database is not a version this service reads\n");
        return false;
    }
    uint32_t const count = header[2];
    uint64_t const wanted =
        aegir::authdb::kHeaderBytes + static_cast<uint64_t>(count) * sizeof(aegir::authdb::Row);
    if (wanted > users_length) {
        write("  memory: the user database is shorter than its header promises\n");
        return false;
    }
    if (count == 0) {
        return true;
    }
    g_rules = static_cast<UserRule *>(arena.allocate(sizeof(UserRule) * count));
    g_committed = static_cast<uint64_t *>(arena.allocate(sizeof(uint64_t) * count));
    if (g_rules == nullptr || g_committed == nullptr) {
        return false;
    }
    auto const *rows =
        reinterpret_cast<aegir::authdb::Row const *>(users + aegir::authdb::kHeaderBytes);
    for (uint32_t i = 0; i < count; ++i) {
        aegir::limits::View const name{
            rows[i].name, field_length(rows[i].name, aegir::authdb::kNameBytes)};
        aegir::limits::View const klass{
            rows[i].klass, field_length(rows[i].klass, aegir::authdb::kClassBytes)};
        aegir::limits::Amount const log = g_limits.resolve(name, klass,
                                                           aegir::limits::Resource::Memory,
                                                           aegir::limits::Action::Log);
        aegir::limits::Amount const deny = g_limits.resolve(name, klass,
                                                            aegir::limits::Resource::Memory,
                                                            aegir::limits::Action::Deny);
        g_rules[i] = UserRule{log, deny};
        g_committed[i] = 0;
    }
    g_user_count = count;
    return true;
}

void answer_alloc(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                  uint64_t badge) noexcept
{
    uint64_t const wanted_bits =
        count >= 1 && words[0] != 0 ? words[0] : aegir::memory::kChunkBits;
    if (wanted_bits == 0 || wanted_bits > g_pool_bits) {
        port.reply_words(nullptr, 0);
        return;
    }

    /* A user badge is limited by its class and name; a system badge is the
     * superuser and is not (specs/authority.md, specs/limits.md). */
    bool limited = false;
    uint64_t user = 0;
    if (aegir::ipc::is_user_badge(badge)) {
        user = aegir::ipc::user_index(badge);
        limited = user < g_user_count && g_rules != nullptr;
    }
    uint64_t const bytes = 1ull << wanted_bits;
    if (limited) {
        if (aegir::limits::crosses(g_rules[user].deny, g_committed[user], bytes)) {
            write("  memory: denied a chunk to user ");
            write_word(user);
            write(", over its deny limit\n");
            port.reply_words(nullptr, 0);
            return;
        }
        if (aegir::limits::crosses(g_rules[user].log, g_committed[user], bytes)) {
            write("  memory: user ");
            write_word(user);
            write(" crossed its log limit, ");
            write_word(g_committed[user] + bytes);
            write(" bytes committed\n");
        }
    }

    uint32_t const index = chunk_free();
    if (index == kMaxChunks) {
        port.reply_words(nullptr, 0);
        return;
    }
    seL4_Error error = seL4_NoError;
    aegir::mem::Account account{"chunk", 0, 0, 0};
    void *cookie = nullptr;
    seL4_CPtr const chunk = g_pool.carve_untyped(static_cast<seL4_Word>(wanted_bits),
                                                 account, &error, nullptr, &cookie);
    if (chunk == 0) {
        port.reply_words(nullptr, 0);
        return;
    }
    g_chunk[index] = Chunk{chunk, cookie, badge, static_cast<uint32_t>(wanted_bits)};
    if (limited) {
        g_committed[user] += bytes;
    }
    uint64_t answer[1] = {wanted_bits};
    port.reply_cap(answer, 1, chunk);
}

void answer_release(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                    uint64_t badge) noexcept
{
    uint64_t const owner = count >= 1 && words[0] != 0 ? words[0] : badge;
    uint64_t const released = release_owner(owner);
    port.reply_words(&released, 1);
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

    /* The pool: the untyped director delegated, its size the boot's, not ours. */
    uint64_t pool_slot = 0;
    uint64_t pool_physical = 0;
    uint32_t pool_bits = 0;
    uint64_t pool_address = 0;
    if (!aegir::bootstrap::capability("untyped", 7, &pool_slot) ||
        !aegir::bootstrap::untyped(&pool_physical, &pool_bits, &pool_address) ||
        pool_bits <= aegir::memory::kChunkBits) {
        write("  memory: no pool to carve\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    g_pool_bits = pool_bits;

    /* The slots past the block's names are ours; the pool draws them from a
     * free list so carving and freeing chunks reuses them. */
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
    uint32_t const slot_count =
        (1u << aegir::bootstrap::kCNodeBits) - static_cast<uint32_t>(first_free);
    g_slots.adopt(static_cast<seL4_CPtr>(first_free),
                  slot_count < kMaxSlots ? slot_count : kMaxSlots, g_slot_owners);
    g_pool.adopt_nodes(g_nodes, sizeof(g_nodes));
    g_pool.adopt_slots(0, 0, 0);
    g_pool.set_cnode_size_bits(aegir::bootstrap::kCNodeBits);
    g_pool.adopt_slot_pool(&g_slots, kPoolOwner);
    if (!g_pool.adopt_untyped(static_cast<seL4_CPtr>(pool_slot), pool_bits, pool_physical)) {
        write("  memory: the pool would not be adopted\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    /* Trace the pool's chunks while the "0 bytes available" hunt is on
     * (specs/memory.md). */
    g_pool.set_trace(memory_trace, nullptr);

    /* The window this service reads its configuration through: the service was
     * given its own VSpace and a window of free addresses (maps), and the
     * arena that holds the parsed database and limits maps pages from there. */
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    if (!aegir::bootstrap::capability("vspace", 6, &vspace_slot) ||
        !aegir::bootstrap::window(&window_base, &window_bytes) ||
        !g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                         static_cast<uintptr_t>(window_base),
                         static_cast<uintptr_t>(window_base + window_bytes), &g_pool)) {
        write("  memory: no window to read its configuration through\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    write("  memory: pool ready, ");
    write_word(1ull << (pool_bits - 20));
    write(" MiB, chunks of 2 MiB\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    /* The limits (specs/limits.md): read once, resolved per user, and read
     * after the pool is announced so a filesystem that is still coming up does
     * not delay the boot set. A boot service that reads Sys: retries, the
     * idiom auth uses for C: -- and while it waits, nothing user-badged is
     * allocating yet. */
    aegir::mem::Account config_account{"memory-config", 0, 0, 0};
    aegir::mem::Arena arena(g_pool, g_scratch, config_account);
    aegir::vfs::Namespace space = aegir::vfs::Namespace::find();
    if (space.valid() && load_config(space, arena)) {
        write("  memory: limits from Sys:S/limits.manifest, ");
        write_word(g_user_count);
        write(" user(s)\n");
    } else {
        write("  memory: no limits loaded; the machine is the limit\n");
    }

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
