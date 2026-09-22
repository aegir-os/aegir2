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
#include <aegir/volume.h>

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

/* One member of a binding: the volume it stands for and the volume-relative
 * rest, both resolved when the member was bound, plus the flags that bind
 * carried (specs/namespace.md). A plain alias has one member; a union has an
 * ordered list, and the order is the search order. Resolving at bind is what
 * lets a member be a per-badge alias (`ENV:`'s members are
 * `Home:Prefs/Env-Archive`) without every later call knowing the badge, and
 * it is what pins the binding to a volume rather than to a name. */
struct Member {
    Volume const *volume;
    char rest[aegir::nmspace::kPathMax];
    uint32_t rest_length;
    uint64_t flags;
    Member *next;
};

/* An alias: a name that stands for an ordered list of paths (specs/vfs.md's
 * Aliases, specs/namespace.md's union). Sys is kept as one of these with the
 * everyone-badge rather than through a shape of its own -- one table, one
 * walk. */
struct Binding {
    uint64_t badge; /* whose alias this is; kAliasEveryone for a global */
    uint64_t flags; /* kBindAppend / kBindPrepend / kBindCreate (specs/namespace.md) */
    uint32_t union_id; /* the id its union cap carries, when it has more than one member */
    char name[aegir::nmspace::kNameMax];
    uint32_t name_length;
    Member *members; /* the ordered list; never empty for a live binding */
    Binding *next;
};
constexpr uint64_t kAliasEveryone = ~0ULL;
Binding *g_bindings = nullptr;
uint32_t g_binding_count = 0;
/* The next union id: a binding that grows past one member becomes a union, and
 * its cap's badge carries this (specs/namespace.md). Ids are not reused -- a
 * stale cap names a union that is gone, which is a refusal, not a wrong read. */
uint32_t g_union_next = 1;
/* Dropped bindings and members, for reuse: each is one size, so an unbound
 * row serves the next bind, and the arena's bound is the most ever live at
 * once rather than ever made (specs/vfs.md). */
Binding *g_binding_free = nullptr;
Member *g_member_free = nullptr;

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

Member *member_take() noexcept
{
    Member *member = g_member_free;
    if (member != nullptr) {
        g_member_free = member->next;
        return member;
    }
    return static_cast<Member *>(arena_take(sizeof(Member)));
}

void member_free(Member *member) noexcept
{
    member->next = g_member_free;
    g_member_free = member;
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
            Member *member = member_take();
            if (member == nullptr) {
                write("  vfs: no room for the Sys: alias, refused\n");
            } else {
                sys->badge = kAliasEveryone;
                sys->flags = 0;
                sys->name[0] = 'S';
                sys->name[1] = 'y';
                sys->name[2] = 's';
                sys->name_length = 3;
                member->volume = volume;
                member->rest_length = 0;
                member->flags = 0;
                member->next = nullptr;
                sys->members = member;
                sys->next = g_bindings;
                g_bindings = sys;
                ++g_binding_count;
                write("  vfs: Sys: is ");
                write(volume->name, volume->name_length);
                write("\n");
            }
        }
    }
    uint64_t answer[aegir::nmspace::kNameMax / 8 + 1];
    uint32_t const answer_words =
        aegir::nmspace::pack_string(answer, volume->name, volume->name_length,
                                    aegir::nmspace::kNameMax);
    port.reply_words(answer, answer_words);
}

/* The outcome of resolving a path's leading `Name:`: a volume, a union, or
 * nothing. `rest_length` counts the bytes after the colon that landed in the
 * caller's buffer -- what the name truly resolves to. */
enum class Resolved { None, Volume, Union };

struct Resolution {
    Resolved kind;
    Volume const *volume;   /* when kind == Volume */
    Binding const *binding; /* when kind == Union */
    uint32_t rest_length;
};

/* Defined below, beside the walk it shares with resolve: `bind` resolves a
 * member's path when it is bound, so the member pins a volume rather than a
 * name (specs/namespace.md). */
Resolution resolve_path(uint64_t badge, char const *path, uint32_t path_length,
                        char *rest, uint32_t rest_capacity) noexcept;

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
     * dead on arrival. A second bind of a *name* grows the list or replaces it
     * (specs/namespace.md): append and prepend add a member, the default
     * replaces, which is the one-member alias specs/vfs.md describes. */
    bool colon = false;
    for (uint32_t i = 0; i < name_length; ++i) {
        if (name[i] == ':') {
            colon = true;
        }
    }
    if (colon || find_volume(name, name_length) != nullptr) {
        port.reply_words(nullptr, 0);
        return;
    }
    /* Resolve the path now, with the binder's badge: a member can be a
     * per-badge alias (`ENV:`'s `Home:Prefs/Env-Archive`), and resolving here
     * is what pins the member to a volume (specs/namespace.md). */
    static char member_rest[aegir::nmspace::kPathMax];
    Resolution const resolved =
        resolve_path(badge, target, target_length, member_rest, sizeof(member_rest));
    if (resolved.kind != Resolved::Volume) {
        write("  vfs: a bind whose path names no volume, refused\n");
        port.reply_words(nullptr, 0);
        return;
    }
    Member *member = member_take();
    if (member == nullptr) {
        write("  vfs: no room for another alias, refused\n");
        port.reply_words(nullptr, 0);
        return;
    }
    member->volume = resolved.volume;
    for (uint32_t i = 0; i < resolved.rest_length; ++i) {
        member->rest[i] = member_rest[i];
    }
    member->rest_length = resolved.rest_length;
    member->flags = flags;
    member->next = nullptr;

    Binding *binding = find_binding(badge, name, name_length);
    if (binding != nullptr) {
        if ((flags & aegir::nmspace::kBindAppend) != 0) {
            Member **tail = &binding->members;
            while (*tail != nullptr) {
                tail = &(*tail)->next;
            }
            *tail = member;
        } else if ((flags & aegir::nmspace::kBindPrepend) != 0) {
            member->next = binding->members;
            binding->members = member;
        } else {
            while (binding->members != nullptr) {
                Member *old = binding->members;
                binding->members = old->next;
                member_free(old);
            }
            binding->members = member;
        }
        binding->flags = flags;
        if (binding->members->next != nullptr && binding->union_id == 0) {
            binding->union_id = g_union_next++;
        }
        write("  vfs: ");
        write(binding->name, binding->name_length);
        write(": grown\n");
        uint64_t const one = 1;
        port.reply_words(&one, 1);
        return;
    }

    Binding *fresh = g_binding_free;
    if (fresh != nullptr) {
        g_binding_free = fresh->next;
    } else {
        fresh = static_cast<Binding *>(arena_take(sizeof(Binding)));
    }
    if (fresh == nullptr) {
        member_free(member);
        write("  vfs: no room for another alias, refused\n");
        port.reply_words(nullptr, 0);
        return;
    }
    fresh->badge = badge;
    fresh->flags = flags;
    fresh->union_id = 0;
    for (uint32_t i = 0; i < name_length; ++i) {
        fresh->name[i] = name[i];
    }
    fresh->name_length = name_length;
    fresh->members = member;
    fresh->next = g_bindings;
    g_bindings = fresh;
    ++g_binding_count;
    write("  vfs: ");
    write(fresh->name, fresh->name_length);
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
            while (b->members != nullptr) {
                Member *member = b->members;
                b->members = member->next;
                member_free(member);
            }
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

/* Compose a member's volume-relative path: the member's own rest, then a
 * slash, then the caller's rest (specs/namespace.md). An empty piece adds
 * nothing. False when the result outgrows the path bound. */
bool compose_path(char const *member, uint32_t member_length, char const *rest,
                  uint32_t rest_length, char *out, uint32_t *out_length) noexcept
{
    uint32_t const separator = member_length != 0 && rest_length != 0 ? 1 : 0;
    uint32_t const length = member_length + separator + rest_length;
    if (length > aegir::nmspace::kPathMax) {
        return false;
    }
    uint32_t at = 0;
    for (uint32_t i = 0; i < member_length; ++i) {
        out[at++] = member[i];
    }
    if (separator != 0) {
        out[at++] = '/';
    }
    for (uint32_t i = 0; i < rest_length; ++i) {
        out[at++] = rest[i];
    }
    *out_length = length;
    return true;
}

/* Resolve `path`'s leading `Name:` to a volume: a volume's own name answers
 * with its rest, and an alias answers with its first member's volume and the
 * member's rest composed with the caller's. The member was resolved when it
 * was bound (specs/namespace.md), so one hop is all it takes -- a member can
 * be a per-badge alias (`ENV:`'s `Home:Prefs/Env-Archive`) without this call
 * knowing the badge, and the binding is pinned to a volume, not a name. A
 * union answers with its binding and the caller's rest; the union's cap is
 * the caller's to mint. The rest lands in `rest` (up to `rest_capacity`); a
 * path that names no volume is `Resolved::None`. */
Resolution resolve_path(uint64_t badge, char const *path, uint32_t path_length,
                        char *rest, uint32_t rest_capacity) noexcept
{
    uint32_t colon = 0;
    while (colon < path_length && path[colon] != ':') {
        ++colon;
    }
    if (colon == path_length || colon == 0) {
        return {Resolved::None, nullptr, nullptr, 0};
    }
    Volume const *volume = find_volume(path, colon);
    if (volume != nullptr) {
        uint32_t const rest_length = path_length - colon - 1;
        if (rest_length > rest_capacity) {
            return {Resolved::None, nullptr, nullptr, 0};
        }
        for (uint32_t i = 0; i < rest_length; ++i) {
            rest[i] = path[colon + 1 + i];
        }
        return {Resolved::Volume, volume, nullptr, rest_length};
    }
    Binding const *binding = find_binding(badge, path, colon);
    if (binding == nullptr) {
        write("  vfs: ");
        write(path, colon);
        write(": no such volume\n");
        return {Resolved::None, nullptr, nullptr, 0};
    }
    if (binding->union_id != 0) {
        uint32_t const rest_length = path_length - colon - 1;
        if (rest_length > rest_capacity) {
            return {Resolved::None, nullptr, nullptr, 0};
        }
        for (uint32_t i = 0; i < rest_length; ++i) {
            rest[i] = path[colon + 1 + i];
        }
        return {Resolved::Union, nullptr, binding, rest_length};
    }
    Member const *member = binding->members;
    uint32_t rest_length = 0;
    if (!compose_path(member->rest, member->rest_length, path + colon + 1,
                      path_length - colon - 1, rest, &rest_length) ||
        rest_length > rest_capacity) {
        return {Resolved::None, nullptr, nullptr, 0};
    }
    return {Resolved::Volume, member->volume, nullptr, rest_length};
}

/* resolve: a `Volume:rest` path, where the volume part may be an alias
 * (specs/vfs.md's Aliases). Substitution composes a path the caller never
 * wrote -- Home:WELCOME.TXT is Sys:Homes/<user>/WELCOME.TXT is
 * AEGIR:Homes/<user>/WELCOME.TXT -- so the reply carries the
 * volume-relative rest as a string, and the alias table's own length bounds
 * the chain: a chain that outlasts it is a cycle. The walk is resolve_path's,
 * shared with the union's forwarding; the rest buffer here is static, as the
 * serve loop is one thread. */
void answer_resolve(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count,
                    seL4_Word badge) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                       &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }

    static char rest[aegir::nmspace::kPathMax];
    Resolution const resolved = resolve_path(badge, path, path_length, rest, sizeof(rest));
    if (resolved.kind == Resolved::None) {
        port.reply_words(nullptr, 0);
        return;
    }
    if (resolved.kind == Resolved::Union) {
        /* A union: the answer is the union cap -- a minted copy of *this*
         * endpoint carrying the union's id -- and the caller's own rest,
         * with no substitution (specs/namespace.md). The VFS's one receive
         * tells a union call from a namespace call by the badge. */
        if (seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, g_mint_slot,
                            aegir::bootstrap::kCNodeBits,
                            aegir::bootstrap::kSlotOwnCNode, port.capability(),
                            aegir::bootstrap::kCNodeBits,
                            seL4_CapRights_new(1, 1, 0, 1),
                            aegir::nmspace::union_badge(resolved.binding->union_id)) !=
            seL4_NoError) {
            write("  vfs: a union's cap would not mint\n");
            port.reply_words(nullptr, 0);
            return;
        }
        uint64_t answer[aegir::nmspace::kResolveWords];
        uint32_t const answer_words = aegir::nmspace::pack_string(
            answer, rest, resolved.rest_length, aegir::nmspace::kPathMax);
        port.reply_cap(answer, answer_words, g_mint_slot);
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_mint_slot,
                          aegir::bootstrap::kCNodeBits);
        return;
    }

    /* The caller's own badge on the copy: the filesystem learns who is
     * asking from the kernel, which is what a range or a permission will one
     * day clamp by (specs/vfs.md). Minting from the unbadged stored cap is
     * what makes this possible at all -- a badged cap cannot be minted
     * again. */
    if (seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, g_mint_slot,
                        aegir::bootstrap::kCNodeBits, aegir::bootstrap::kSlotOwnCNode,
                        resolved.volume->port, aegir::bootstrap::kCNodeBits,
                        seL4_CapRights_new(1, 0, 0, 1), badge) != seL4_NoError) {
        write("  vfs: a resolve's badge would not mint\n");
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t answer[aegir::nmspace::kResolveWords];
    uint32_t const answer_words = aegir::nmspace::pack_string(
        answer, rest, resolved.rest_length, aegir::nmspace::kPathMax);
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

/* The binding a union id names: the id is what the union cap's badge carries,
 * and a stale id -- one whose binding is gone -- names nothing, which is a
 * refusal rather than a wrong read (specs/namespace.md). */
Binding const *find_union(uint32_t id) noexcept
{
    for (Binding const *b = g_bindings; b != nullptr; b = b->next) {
        if (b->union_id == id) {
            return b;
        }
    }
    return nullptr;
}

/* Mint a member volume's stored cap with the caller's badge into the scratch
 * slot, so the member sees the true caller and not the VFS
 * (specs/namespace.md). Zero when the kernel refuses; the slot answers one
 * nested call and is dropped after. */
seL4_CPtr mint_member(Volume const *volume, uint64_t badge) noexcept
{
    if (seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, g_mint_slot,
                        aegir::bootstrap::kCNodeBits, aegir::bootstrap::kSlotOwnCNode,
                        volume->port, aegir::bootstrap::kCNodeBits,
                        seL4_CapRights_new(1, 0, 0, 1), badge) != seL4_NoError) {
        return 0;
    }
    return g_mint_slot;
}

void drop_member() noexcept
{
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_mint_slot,
                      aegir::bootstrap::kCNodeBits);
}

/* One member's list entry: compose the volume-relative path and ask the
 * member's volume for entry `index`. The answer's words land in `out`; the
 * return is their count, zero when the member has no such entry or the nested
 * call failed. The serve loop is one thread, so the working buffers are
 * static. */
uint32_t member_list_entry(Member const *member, uint64_t caller, char const *path,
                           uint32_t path_length, uint64_t index, uint64_t *out) noexcept
{
    static char member_path[aegir::nmspace::kPathMax];
    static uint64_t payload[aegir::ipc::kMaxWords];

    uint32_t member_path_length = 0;
    if (!compose_path(member->rest, member->rest_length, path, path_length, member_path,
                      &member_path_length)) {
        return 0;
    }
    uint32_t const member_words = aegir::nmspace::pack_string(
        payload, member_path, member_path_length, aegir::nmspace::kPathMax);
    if (member_words == 0 || member_words + 1 > aegir::ipc::kMaxWords) {
        return 0;
    }
    payload[member_words] = index;
    seL4_CPtr const cap = mint_member(member->volume, caller);
    if (cap == 0) {
        return 0;
    }
    aegir::ipc::Consumer const consumer(cap);
    aegir::ipc::WordsReply const reply = consumer.call_words(
        aegir::volume::kMethodList, payload, member_words + 1, out, aegir::ipc::kMaxWords);
    drop_member();
    if (reply.error != 0) {
        return 0;
    }
    return reply.count;
}

/* Whether a list answer's leading name is `name`. */
bool answer_name_is(uint64_t const *answer, uint32_t count, char const *name,
                    uint32_t name_length) noexcept
{
    char const *entry = nullptr;
    uint32_t entry_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(answer, count, aegir::nmspace::kNameMax, &entry,
                                       &entry_length)) {
        return false;
    }
    return same_volume(entry, entry_length, name, name_length);
}

/* read, forwarded: the first member that has the path answers
 * (specs/namespace.md). The read's path is union-relative; each member's is
 * its rest composed with it. The member's answer -- count, end-of-file, bytes
 * -- is the union's, word for word. */
void union_read(aegir::ipc::Owner &port, Binding const *binding, uint64_t caller,
                uint64_t const *words, uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count < 1 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                       &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint32_t const path_words = 1 + (path_length + 7) / 8;
    if (count < path_words + 2) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const offset = words[path_words];
    uint64_t const max = words[path_words + 1];

    static char member_path[aegir::nmspace::kPathMax];
    static uint64_t payload[aegir::ipc::kMaxWords];
    static uint64_t answer[aegir::ipc::kMaxWords];

    for (Member const *m = binding->members; m != nullptr; m = m->next) {
        uint32_t member_path_length = 0;
        if (!compose_path(m->rest, m->rest_length, path, path_length, member_path,
                          &member_path_length)) {
            continue;
        }
        uint32_t const member_words = aegir::nmspace::pack_string(
            payload, member_path, member_path_length, aegir::nmspace::kPathMax);
        if (member_words == 0 || member_words + 2 > aegir::ipc::kMaxWords) {
            continue;
        }
        payload[member_words] = offset;
        payload[member_words + 1] = max;
        seL4_CPtr const cap = mint_member(m->volume, caller);
        if (cap == 0) {
            continue;
        }
        aegir::ipc::Consumer const consumer(cap);
        aegir::ipc::WordsReply const reply = consumer.call_words(
            aegir::volume::kMethodRead, payload, member_words + 2, answer,
            aegir::ipc::kMaxWords);
        drop_member();
        if (reply.error == 0 && reply.count > 0) {
            port.reply_words(answer, reply.count);
            return;
        }
    }
    port.reply_words(nullptr, 0);
}

/* list, forwarded and merged: every member's entries for the path, in order,
 * with a name an earlier member returned not returned again
 * (specs/namespace.md). The caller's index is the cursor, and the directory
 * owes no stability across calls, so a call rebuilds the merged sequence up to
 * the index it is asked for rather than keeping one. */
void union_list(aegir::ipc::Owner &port, Binding const *binding, uint64_t caller,
                uint64_t const *words, uint32_t count) noexcept
{
    char const *path = nullptr;
    uint32_t path_length = 0;
    if (count < 1 ||
        !aegir::nmspace::unpack_string(words, count, aegir::nmspace::kPathMax, &path,
                                       &path_length)) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint32_t const path_words = 1 + (path_length + 7) / 8;
    if (count < path_words + 1) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const want = words[path_words];

    static uint64_t entry[aegir::ipc::kMaxWords];
    static uint64_t earlier[aegir::ipc::kMaxWords];

    uint64_t unique = 0;
    for (Member const *m = binding->members; m != nullptr; m = m->next) {
        for (uint64_t j = 0;; ++j) {
            uint32_t const entry_count =
                member_list_entry(m, caller, path, path_length, j, entry);
            if (entry_count == 0) {
                break;
            }
            char const *name = nullptr;
            uint32_t name_length = 0;
            if (!aegir::nmspace::unpack_string(entry, entry_count, aegir::nmspace::kNameMax,
                                               &name, &name_length)) {
                break;
            }
            /* A name an earlier member returned is shadowed: scan the members
             * before this one. */
            bool shadowed = false;
            for (Member const *p = binding->members; p != m && !shadowed; p = p->next) {
                for (uint64_t k = 0;; ++k) {
                    uint32_t const earlier_count =
                        member_list_entry(p, caller, path, path_length, k, earlier);
                    if (earlier_count == 0) {
                        break;
                    }
                    if (answer_name_is(earlier, earlier_count, name, name_length)) {
                        shadowed = true;
                        break;
                    }
                }
            }
            if (shadowed) {
                continue;
            }
            if (unique == want) {
                port.reply_words(entry, entry_count);
                return;
            }
            ++unique;
        }
    }
    port.reply_words(nullptr, 0);
}

/* A union's volume call (specs/namespace.md): the badge names the union, and
 * the caller's identity is the first word -- the badge is spent on the union
 * id, so the identity cannot ride there. read and list are served here; the
 * write side (open, write, close, mkdir, remove) is the next piece, and a
 * method this version does not know is answered by saying nothing. */
void answer_union(aegir::ipc::Owner &port, uint64_t badge, uint32_t method,
                  uint64_t const *words, uint32_t count) noexcept
{
    Binding const *binding = find_union(aegir::nmspace::union_id(badge));
    if (binding == nullptr || count < 1) {
        port.reply_words(nullptr, 0);
        return;
    }
    uint64_t const caller = words[0];
    uint64_t const *payload = words + 1;
    uint32_t const payload_count = count - 1;
    switch (method) {
    case aegir::volume::kMethodRead:
        union_read(port, binding, caller, payload, payload_count);
        break;
    case aegir::volume::kMethodList:
        union_list(port, binding, caller, payload, payload_count);
        break;
    default:
        port.reply_words(nullptr, 0);
        break;
    }
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
        if (aegir::nmspace::is_union(badge)) {
            answer_union(port, badge, method, words, count);
            continue;
        }
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
