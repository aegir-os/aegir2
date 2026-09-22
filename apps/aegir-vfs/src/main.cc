/*
 * Aegir's VFS: the namespace, and nothing else.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * It owns `vfs.namespace` (specs/vfs.md) and keeps the map: volume name to
 * the port capability of the filesystem that serves it. Files are not here;
 * the map is. Three questions, per the protocol (aegir/nmspace.h):
 *
 *   - register: a name, flags, and the volume port's unbadged caller half,
 *     which arrives in the scratch receive slot and is moved out at once. A
 *     duplicate name gains a `_N` suffix, and the answer is the name the
 *     volume actually got.
 *   - resolve: a `Volume:rest` path. The answer is the volume's port minted
 *     with the caller's badge -- the filesystem sees the true caller, not the
 *     VFS -- and the byte where `rest` begins. The minted copy is deleted
 *     after the reply: the kernel transferred a copy, and the slot answers
 *     the next resolve.
 *   - count/describe: the map, a row at a time.
 *
 * The table is a list over the memory the manifest granted, and it grows
 * until that memory is gone -- the bound is a grant, declared in one place,
 * and reaching it is a loud failure, never a quiet overwrite.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/nmspace.h>

#include <sel4/sel4.h>

namespace {

void write(char const *text) noexcept
{
    aegir::debug_write(text);
}

void write(char const *text, uint32_t length) noexcept
{
    aegir::debug_write(text, length);
}

struct Volume {
    char name[aegir::nmspace::kNameMax];
    uint32_t name_length;
    uint64_t flags;
    seL4_CPtr port; /* the caller half, unbadged; each resolve mints from it */
    Volume *next;
};

Volume *g_volumes = nullptr;
uint32_t g_volume_count = 0;

/* An alias: a name that stands for a path (specs/vfs.md's Aliases). Sys is
 * kept as one of these with the everyone-badge rather than through a shape
 * of its own -- one table, one walk. */
struct Binding {
    uint64_t badge; /* whose alias this is; kAliasEveryone for a global */
    uint64_t flags; /* kBindAppend / kBindPrepend / kBindCreate (specs/namespace.md) */
    char name[aegir::nmspace::kNameMax];
    uint32_t name_length;
    char target[aegir::nmspace::kPathMax];
    uint32_t target_length;
    Binding *next;
};
constexpr uint64_t kAliasEveryone = ~0ULL;
Binding *g_bindings = nullptr;
uint32_t g_binding_count = 0;
/* Dropped bindings, for reuse: every binding is the same size, so an
 * unbound row serves the next bind, and the arena's bound is the most
 * aliases ever live at once rather than ever made -- the demand the
 * session-reclaim arc creates, met by reuse (specs/vfs.md). */
Binding *g_binding_free = nullptr;

/* The table's backing store: the memory the manifest's memory_kib granted,
 * mapped and ours, used up from the front. */
uint8_t *g_arena = nullptr;
uint64_t g_arena_left = 0;

void *arena_take(uint64_t bytes) noexcept
{
    bytes = (bytes + 7) & ~7ULL;
    if (bytes > g_arena_left) {
        return nullptr;
    }
    void *taken = g_arena;
    g_arena += bytes;
    g_arena_left -= bytes;
    return taken;
}

/* Slots past everything the bootstrap block names are ours (the adoption the
 * other services do, specs/authority.md): stored caps fill upward, and one
 * slot above them is the resolve mint's scratch. */
seL4_CPtr g_next_slot = 0;
seL4_CPtr g_mint_slot = 0;

char folded(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool same_volume(char const *a, uint32_t a_length, char const *b, uint32_t b_length) noexcept
{
    if (a_length != b_length) {
        return false;
    }
    for (uint32_t i = 0; i < a_length; ++i) {
        if (folded(a[i]) != folded(b[i])) {
            return false;
        }
    }
    return true;
}

Volume *find_volume(char const *name, uint32_t length) noexcept
{
    for (Volume *v = g_volumes; v != nullptr; v = v->next) {
        if (same_volume(v->name, v->name_length, name, length)) {
            return v;
        }
    }
    return nullptr;
}

/* The binding that answers a name for a caller: the caller's own first, the
 * everyone-badge after -- a session's Home: is its own, and Sys: is
 * everyone's. */
Binding *find_binding(uint64_t badge, char const *name, uint32_t length) noexcept
{
    Binding *global = nullptr;
    for (Binding *b = g_bindings; b != nullptr; b = b->next) {
        if (!same_volume(b->name, b->name_length, name, length)) {
            continue;
        }
        if (b->badge == badge) {
            return b;
        }
        if (b->badge == kAliasEveryone) {
            global = b;
        }
    }
    return global;
}

/* The name the volume actually gets: the one it asked for, or that with a
 * `_N` suffix when some volume has it already (specs/vfs.md). False when the
 * name leaves no room for a suffix. */
bool assign_name(char *out, uint32_t *out_length, char const *wanted, uint32_t wanted_length) noexcept
{
    if (find_volume(wanted, wanted_length) == nullptr) {
        for (uint32_t i = 0; i < wanted_length; ++i) {
            out[i] = wanted[i];
        }
        *out_length = wanted_length;
        return true;
    }
    for (uint32_t n = 1;; ++n) {
        uint32_t length = wanted_length;
        if (length + 2 > aegir::nmspace::kNameMax) {
            return false;
        }
        out[length++] = '_';
        uint32_t const number_at = length;
        for (uint32_t m = n;; m /= 10) {
            if (length + 1 > aegir::nmspace::kNameMax) {
                return false;
            }
            out[length++] = static_cast<char>('0' + m % 10);
            if (m < 10) {
                break;
            }
        }
        for (uint32_t i = number_at, j = length - 1; i < j; ++i, --j) {
            char const swap = out[i];
            out[i] = out[j];
            out[j] = swap;
        }
        if (find_volume(out, length) == nullptr) {
            *out_length = length;
            return true;
        }
    }
}

void answer_register(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                     bool cap_arrived) noexcept
{
    char const *name = nullptr;
    uint32_t name_length = 0;
    uint32_t const name_words =
        count > 0 ? 1 + static_cast<uint32_t>((words[0] + 7) / 8) : 0;
    uint64_t const flags = count > name_words ? words[name_words] : 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kNameMax, &name,
                                       &name_length) ||
        name_length == 0 || !cap_arrived) {
        write("  vfs: a register with no name or no capability, refused\n");
        port.reply_words(nullptr, 0);
        return;
    }
    seL4_CPtr const stored = g_next_slot;
    Volume *volume = static_cast<Volume *>(arena_take(sizeof(Volume)));
    if (volume == nullptr || !aegir::ipc::take_received_cap(stored)) {
        write("  vfs: no room for another volume, refused\n");
        port.reply_words(nullptr, 0);
        return;
    }
    if (!assign_name(volume->name, &volume->name_length, name, name_length)) {
        write("  vfs: a volume name with no room for a suffix, refused\n");
        port.reply_words(nullptr, 0);
        return;
    }
    volume->flags = flags;
    volume->port = stored;
    volume->next = g_volumes;
    g_volumes = volume;
    ++g_volume_count;
    ++g_next_slot;

    write("  vfs: ");
    write(volume->name, volume->name_length);
    write(": registered\n");
    if ((flags & aegir::nmspace::kFlagBoot) != 0) {
        Binding *sys = static_cast<Binding *>(arena_take(sizeof(Binding)));
        if (sys == nullptr) {
            write("  vfs: no room for the Sys: alias, refused\n");
        } else if (find_binding(kAliasEveryone, "Sys", 3) != nullptr) {
            write("  vfs: ");
            write(volume->name, volume->name_length);
            write(": a second boot flag -- the first stands\n");
        } else {
            sys->badge = kAliasEveryone;
            sys->flags = 0;
            sys->name[0] = 'S';
            sys->name[1] = 'y';
            sys->name[2] = 's';
            sys->name_length = 3;
            for (uint32_t i = 0; i < volume->name_length; ++i) {
                sys->target[i] = volume->name[i];
            }
            sys->target_length = volume->name_length;
            sys->next = g_bindings;
            g_bindings = sys;
            ++g_binding_count;
            write("  vfs: Sys: is ");
            write(sys->target, sys->target_length);
            write("\n");
        }
    }
    uint64_t answer[aegir::nmspace::kNameMax / 8 + 1];
    uint32_t const answer_words =
        aegir::nmspace::pack_string(answer, volume->name, volume->name_length,
                                    aegir::nmspace::kNameMax);
    port.reply_words(answer, answer_words);
}

/* bind: a badge, flags, an alias name, the path it stands for
 * (specs/vfs.md's Aliases, specs/namespace.md's union). A pair binds once -- a
 * badge's serial is never reused, so a second bind of the same pair is a lie,
 * not a correction; the flags decide how a *name* with a second member grows. */
void answer_bind(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    char const *name = nullptr;
    uint32_t name_length = 0;
    char const *target = nullptr;
    uint32_t target_length = 0;
    if (count < 3 ||
        !aegir::nmspace::unpack_string(words + 2, count - 2, aegir::nmspace::kNameMax,
                                       &name, &name_length) ||
        name_length == 0) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const badge = words[0];
    uint64_t const flags = words[1];
    uint32_t const name_words = 1 + (name_length + 7) / 8;
    if (count < 2 + name_words ||
        !aegir::nmspace::unpack_string(words + 2 + name_words, count - 2 - name_words,
                                       aegir::nmspace::kPathMax, &target, &target_length) ||
        target_length == 0) {
        port.reply_words(nullptr, 0);
        return;
    }
    /* A name with a colon is a path, not an alias; a volume's name is a
     * volume's -- resolution looks volumes up first, so the alias would be
     * dead on arrival; and a bound pair does not rebind. */
    bool colon = false;
    for (uint32_t i = 0; i < name_length; ++i) {
        if (name[i] == ':') {
            colon = true;
        }
    }
    if (colon || find_volume(name, name_length) != nullptr ||
        find_binding(badge, name, name_length) != nullptr) {
        port.reply_words(nullptr, 0);
        return;
    }
    Binding *binding = g_binding_free;
    if (binding != nullptr) {
        g_binding_free = binding->next;
    } else {
        binding = static_cast<Binding *>(arena_take(sizeof(Binding)));
    }
    if (binding == nullptr) {
        write("  vfs: no room for another alias, refused\n");
        port.reply_words(nullptr, 0);
        return;
    }
    binding->badge = badge;
    binding->flags = flags;
    for (uint32_t i = 0; i < name_length; ++i) {
        binding->name[i] = name[i];
    }
    binding->name_length = name_length;
    for (uint32_t i = 0; i < target_length; ++i) {
        binding->target[i] = target[i];
    }
    binding->target_length = target_length;
    binding->next = g_bindings;
    g_bindings = binding;
    ++g_binding_count;
    write("  vfs: ");
    write(binding->name, binding->name_length);
    write(": bound for badge ");
    aegir::debug_write_hex(badge);
    write("\n");
    uint64_t const one = 1;
    port.reply_words(&one, 1);
}

/* unbind: a badge. Every binding the badge holds is dropped (specs/vfs.md's
 * Aliases) -- the session teardown's mechanism; the answer is how many
 * there were. A dropped row joins the free list: a binding is one size, so
 * the next bind reuses what this let go. */
void answer_unbind(aegir::ipc::Owner &port, uint64_t const *words,
                   uint32_t count) noexcept
{
    uint64_t dropped = 0;
    if (count < 1) {
        port.reply_words(&dropped, 1);
        return;
    }
    uint64_t const badge = words[0];
    Binding **at = &g_bindings;
    while (*at != nullptr) {
        Binding *b = *at;
        if (b->badge == badge) {
            *at = b->next;
            b->next = g_binding_free;
            g_binding_free = b;
            --g_binding_count;
            ++dropped;
        } else {
            at = &b->next;
        }
    }
    port.reply_words(&dropped, 1);
}

/* resolve: a `Volume:rest` path, where the volume part may be an alias
 * (specs/vfs.md's Aliases). Substitution composes a path the caller never
 * wrote -- Home:WELCOME.TXT is Sys:Homes/<user>/WELCOME.TXT is
 * AEGIR:Homes/<user>/WELCOME.TXT -- so the reply carries the
 * volume-relative rest as a string, and the alias table's own length bounds
 * the chain: a chain that outlasts it is a cycle. The buffers are static:
 * the serve loop is one thread, and a path is a kilobyte the stack need
 * not hold twice. */
void answer_resolve(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                    seL4_Word badge) noexcept
{
    static char composed[aegir::nmspace::kPathMax];
    static char next[aegir::nmspace::kPathMax];

    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                       &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint32_t composed_length = path_length;
    for (uint32_t i = 0; i < path_length; ++i) {
        composed[i] = path[i];
    }

    uint32_t budget = g_binding_count + 1;
    Volume const *volume = nullptr;
    uint32_t rest_at = 0;
    for (;;) {
        uint32_t colon = 0;
        while (colon < composed_length && composed[colon] != ':') {
            ++colon;
        }
        if (colon == composed_length || colon == 0) {
            port.reply_words(nullptr, 0);
            return;
        }
        volume = find_volume(composed, colon);
        if (volume != nullptr) {
            rest_at = colon + 1;
            break;
        }
        Binding const *binding = find_binding(badge, composed, colon);
        if (binding == nullptr || budget == 0) {
            write("  vfs: ");
            write(composed, colon);
            write(binding == nullptr ? ": no such volume\n" : ": an alias cycle\n");
            port.reply_words(nullptr, 0);
            return;
        }
        --budget;
        /* Compose: the target, then the rest. A target that names a volume
         * joins with a colon; one that is a path joins with a slash, and an
         * empty rest adds nothing. */
        char const *target = binding->target;
        uint32_t const target_length = binding->target_length;
        uint32_t const rest_length = composed_length - colon - 1;
        bool const target_is_path = [&] {
            for (uint32_t i = 0; i < target_length; ++i) {
                if (target[i] == ':') {
                    return true;
                }
            }
            return false;
        }();
        uint32_t const separator = rest_length != 0 || !target_is_path ? 1 : 0;
        uint32_t const next_length = target_length + separator + rest_length;
        if (next_length > aegir::nmspace::kPathMax) {
            write("  vfs: an alias chain outgrew the path bound\n");
            port.reply_words(nullptr, 0);
            return;
        }
        uint32_t at = 0;
        for (uint32_t i = 0; i < target_length; ++i) {
            next[at++] = target[i];
        }
        if (separator != 0) {
            next[at++] = target_is_path ? '/' : ':';
        }
        for (uint32_t i = 0; i < rest_length; ++i) {
            next[at++] = composed[colon + 1 + i];
        }
        for (uint32_t i = 0; i < next_length; ++i) {
            composed[i] = next[i];
        }
        composed_length = next_length;
    }

    /* The caller's own badge on the copy: the filesystem learns who is
     * asking from the kernel, which is what a range or a permission will one
     * day clamp by (specs/vfs.md). Minting from the unbadged stored cap is
     * what makes this possible at all -- a badged cap cannot be minted
     * again. */
    if (seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, g_mint_slot,
                        aegir::bootstrap::kCNodeBits, aegir::bootstrap::kSlotOwnCNode,
                        volume->port, aegir::bootstrap::kCNodeBits,
                        seL4_CapRights_new(1, 0, 0, 1), badge) != seL4_NoError) {
        write("  vfs: a resolve's badge would not mint\n");
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t answer[aegir::nmspace::kResolveWords];
    uint32_t const answer_words =
        aegir::nmspace::pack_string(answer, composed + rest_at, composed_length - rest_at,
                                    aegir::nmspace::kPathMax);
    port.reply_cap(answer, answer_words, g_mint_slot);
    /* The kernel transferred a copy; ours leaves, and the slot answers the
     * next resolve. */
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_mint_slot,
                      aegir::bootstrap::kCNodeBits);
}

void answer_describe(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    if (count < 1 || words[0] >= g_volume_count) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t index = words[0];
    /* The list is youngest-first; the index a client walks is oldest-first,
     * so that a registration during the walk does not move what was seen. */
    uint32_t const want = g_volume_count - 1 - static_cast<uint32_t>(index);
    Volume const *volume = g_volumes;
    for (uint32_t i = 0; i < want && volume != nullptr; ++i) {
        volume = volume->next;
    }
    if (volume == nullptr) {
        port.reply_words(nullptr, 0);
        return;
    }
    aegir::nmspace::Row row{};
    for (uint32_t i = 0; i < volume->name_length; ++i) {
        row.name[i] = volume->name[i];
    }
    row.flags = volume->flags;
    row.bound = 1;
    port.reply_words(reinterpret_cast<uint64_t const *>(&row), aegir::nmspace::kRowWords);
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Owner port =
        aegir::ipc::Owner::find(aegir::nmspace::kPortName, aegir::nmspace::kPortNameLength);
    if (!port.valid()) {
        write("  vfs: no vfs.namespace port: nothing to own\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The table's backing store: the memory the manifest granted. A service
     * that asked for none keeps an empty map -- registration will refuse,
     * loudly, which is the grant saying what it says. */
    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    if (aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address)) {
        g_arena = reinterpret_cast<uint8_t *>(memory_address);
        g_arena_left = 1ULL << memory_bits;
    }

    /* The slots past the block's names are ours. */
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            if (entry.kind == aegir::bootstrap::EntryKind::Capability &&
                entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            }
        }
    }
    g_next_slot = static_cast<seL4_CPtr>(first_free);
    g_mint_slot = static_cast<seL4_CPtr>(first_free);
    /* The mint scratch must not collide with a stored cap: it takes the first
     * slot, stored caps take the ones after. */
    ++g_next_slot;

    write("  vfs: serving vfs.namespace\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        seL4_Word badge = 0;
        bool cap_arrived = false;
        uint32_t const method =
            port.receive_words(words, aegir::ipc::kMaxWords, &count, &badge, &cap_arrived);
        switch (method) {
        case aegir::nmspace::kMethodRegister:
            answer_register(port, words, count, cap_arrived);
            break;
        case aegir::nmspace::kMethodResolve:
            answer_resolve(port, words, count, badge);
            break;
        case aegir::nmspace::kMethodCount: {
            uint64_t n = g_volume_count;
            port.reply_words(&n, 1);
            break;
        }
        case aegir::nmspace::kMethodDescribe:
            answer_describe(port, words, count);
            break;
        case aegir::nmspace::kMethodBind:
            answer_bind(port, words, count);
            break;
        case aegir::nmspace::kMethodUnbind:
            answer_unbind(port, words, count);
            break;
        default:
            /* A method this version does not know is answered by saying
             * nothing (specs/services.md's versioning rule). */
            port.reply_words(nullptr, 0);
            break;
        }
    }
}
