/*
 * aegir-auth: the user database and the login port (specs/auth.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Reads the packed user table from Initrd: through the namespace -- the
 * first system consumer of the VFS, because the database is bytes on a
 * volume and everything that reads bytes on a volume goes through the map
 * -- and serves auth.login from it: a name and a secret in, one word out,
 * 1 authenticated and 0 refused, with an unknown name, a wrong secret and
 * a malformed call the same 0. A successful login then starts a session:
 * auth is a spawner, its memory is the untyped it was delegated, and the
 * session runs with the user's badge -- the user class bit, the row, and
 * the serial (specs/authority.md). The answer goes first, then the spawn,
 * because the caller's answer must not wait on one. When the session is
 * done -- the supervision wait returns -- auth is the caller who knows it
 * died: the badge's handles are reaped on every volume the namespace
 * names, its aliases unbound, and the pool its objects were retyped from
 * revoked, so the next login starts with the memory and the slots back
 * (specs/auth.md's Session reclaim). No elevation, and the
 * namespace stays open until there is a check to make (specs/auth.md).
 */

#include <aegir/authdb.h>
#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/console.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/vspace.h>
#include <aegir/memory.h>
#include <aegir/metadata.h>
#include <aegir/nmspace.h>
#include <aegir/spawn/initrd.h>
#include <aegir/spawn/kit.h>
#include <aegir/spawn/process.h>
#include <aegir/volume.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

/* The kit every spawner is given (specs/authority.md): the untyped its
 * objects -- and its sessions' -- come out of, its own VSpace root with a
 * window to map through, and the slots past everything the bootstrap
 * block names. The same adoption the partition manager does. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);
aegir::mem::Account g_account{"auth", 0, 0, 0};

/* The delegatable copies the kit carries (specs/services.md): unbadged,
 * because a badged endpoint cap cannot be minted again, and badging is
 * exactly what starting a session takes. */
seL4_CPtr g_spawn_log = 0;
seL4_CPtr g_spawn_nmspace = 0;
/* The memory service, delegatable to the terminals (specs/memory.md Phase 3):
 * a terminal mints a per-command copy from it, so a command's chunks are owned
 * by its own badge and released when it exits. Unbadged like the rest. */
seL4_CPtr g_spawn_mem = 0;
/* The clock, for the session's terminal and its commands: the DOS tools ask
 * the time through it (specs/dos.md), and the shell's Date/Time read it. The
 * timer is the interval side (specs/timer.md): the shell's Wait sleeps on it. */
seL4_CPtr g_spawn_clock = 0;
seL4_CPtr g_spawn_timer = 0;

/* The greeter's kit (specs/console.md's login arc): the delegatable copies
 * its spawn takes, and the console caller half that is auth's own -- a login
 * through the greeter ends with auth reaping the greeter's windows and
 * slice, and the reap call is this half's. The badge is from auth's system
 * children range (specs/authority.md). */
seL4_CPtr g_spawn_gui = 0;
seL4_CPtr g_spawn_login = 0;
/* The owner half of bureau.menu, which the bureau serves (specs/workbench.md):
 * a session is not director's to spawn, so the owner copy the director made
 * reaches the bureau through auth, as the caller copies do. */
seL4_CPtr g_spawn_bureau_menu = 0;
aegir::ipc::Consumer g_gui;
constexpr uint64_t kGreeterBadge = 768;
bool g_greeter_up = false;
/* The greeter's supervision notification: signalled twice -- the form is on
 * the screen (start_greeter's wait), and the accepted login's exit (the
 * login handler's wait, before the reap). */
seL4_CPtr g_greeter_supervision = 0;

/* The boot session (specs/boot.md): the system's Startup-Sequence, run once
 * before the greeter. Its badge is a system child's -- its own console slice
 * and namespace identity. */
constexpr uint64_t kBootBadge = 769;

/* The namespace as auth speaks it, and the slot a home resolve's capability
 * lands in -- one slot, deleted after each use, so a login does not spend
 * what the next one needs. */
aegir::ipc::Consumer g_nmspace;
seL4_CPtr g_home_slot = 0;

/* The session allocator (specs/memory.md, specs/auth.md's Session reclaim): a
 * session's objects and its spawn's staging are retyped from memory the
 * memory service hands out on demand, charged to the session's badge -- there
 * is no fixed pool to size. A login resets the allocator and points its
 * untyped source at a mem.main copy badged for the session; the teardown is
 * one release of that badge, which revokes every chunk it owns (the objects
 * retyped from them go with them). */
aegir::mem::Allocator g_session_mem(nullptr);

/* Its node pool: a session's terminal image splits many pieces (the image
 * nears a megabyte), beyond the allocator's built-in floor. Static, and reset
 * each login. */
alignas(64) unsigned char g_session_nodes[256 * 1024];

/* The mem.main copy the session allocator asks through, badged with the
 * session (minted per login). Zero before the first login. */
seL4_CPtr g_session_mem_call = 0;

/* A reusable slot for the session-badged namespace copy the first-class kit
 * needs (specs/launch.md): a child's namespace must carry the session's
 * identity, so the grant copies a cap badged for the session. It is minted
 * from the unbadged delegate for each terminal, used by the grant, and deleted
 * again. */
seL4_CPtr g_kit_nmspace_slot = 0;

/* Mint a session-badged namespace copy into `slot` (deleting whatever was
 * there): the identity a child's namespace must carry, so Home: and ENV:
 * resolve for it (specs/dos.md). */
bool mint_session_nmspace(seL4_CPtr slot, uint64_t badge) noexcept
{
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, slot,
                      aegir::bootstrap::cnode_bits());
    return seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, slot,
                           aegir::bootstrap::cnode_bits(), aegir::bootstrap::kSlotOwnCNode,
                           g_spawn_nmspace, aegir::bootstrap::cnode_bits(),
                           seL4_CapRights_new(1, 1, 0, 1), badge) == seL4_NoError;
}

/* The untyped source (specs/memory.md): ask mem.main for a chunk as wide as a
 * terminal's runtime, so every carve below -- the bureau's, the terminal's
 * runtime and shell pool -- can split from it. The chunk is owned by the
 * session's badge. */
constexpr uint32_t kSessionChunkBits = 22; /* 4 MiB */

seL4_CPtr session_untyped_source(void *context, seL4_Word *size_bits,
                                 uint64_t *paddr) noexcept
{
    auto *const allocator = static_cast<aegir::mem::Allocator *>(context);
    if (allocator == nullptr || g_session_mem_call == 0) {
        return 0;
    }
    aegir::ipc::Consumer const service(g_session_mem_call);
    uint64_t const request = kSessionChunkBits;
    uint64_t answer[1] = {};
    bool cap_arrived = false;
    aegir::ipc::WordsReply const reply = service.call_transfer(
        aegir::memory::kMethodAlloc, &request, 1, 0, answer, 1, &cap_arrived);
    if (reply.error != 0 || !cap_arrived) {
        aegir::debug_write("      auth: session memory: mem.main refused a chunk\n");
        return 0;
    }
    seL4_CPtr const slot = allocator->alloc_slot();
    if (slot == 0) {
        aegir::debug_write("      auth: session memory: no slot for the chunk\n");
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                          aegir::bootstrap::kSlotReceiveCap,
                          aegir::bootstrap::cnode_bits());
        return 0;
    }
    if (!aegir::ipc::take_received_cap(slot)) {
        aegir::debug_write("      auth: session memory: the chunk cap would not move\n");
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                          aegir::bootstrap::kSlotReceiveCap,
                          aegir::bootstrap::cnode_bits());
        return 0;
    }
    *size_bits = static_cast<seL4_Word>(reply.count >= 1 ? answer[0] : kSessionChunkBits);
    *paddr = 0;
    return slot;
}

/* What a session spawn needs, kept from the bootstrap block: the initrd's
 * bytes (the binary is looked up by name), the ASID pool the address space
 * comes from, and the ends of auth's own CSpace -- the session pool takes its
 * slots from the top down while auth's own objects take theirs from the bottom
 * up, so neither can hand out a slot the other holds (specs/direction.md). */
uint64_t g_binaries_address = 0;
uint32_t g_binaries_bytes = 0;
seL4_CPtr g_asid_pool = 0;
seL4_CPtr g_slots_first = 0;
seL4_CPtr g_slots_end = 0;

aegir::authdb::Row const *g_rows = nullptr;
uint32_t g_users = 0;
/* The serial counts what the user has run (specs/authority.md's badge
 * space): one counter per row, zeroed when the table is read. */
uint32_t *g_serials = nullptr;

void write(char const *text)
{
    aegir::debug_write(text);
}

void write(char const *text, uint32_t length)
{
    aegir::debug_write(text, length);
}

/* Render `value` in decimal into `out`, returning the bytes written (no NUL). */
uint32_t decimal(char *out, uint64_t value) noexcept
{
    char reversed[20];
    uint32_t digits = 0;
    do {
        reversed[digits++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    for (uint32_t i = 0; i < digits; ++i) {
        out[i] = reversed[digits - 1 - i];
    }
    return digits;
}

/* The badge range a session terminal may hand out, as the environment entry
 * the terminal reads at startup (specs/launch.md): `AEGIR_BADGE_RANGE=<base>,
 * <size>`. A static buffer, because the spawn copies it synchronously. */
char const *badge_range_env(uint64_t base, uint64_t size) noexcept
{
    static char buffer[48];
    static char const kPrefix[] = "AEGIR_BADGE_RANGE=";
    uint32_t at = 0;
    for (uint32_t i = 0; i < sizeof(kPrefix) - 1; ++i) {
        buffer[at++] = kPrefix[i];
    }
    at += decimal(buffer + at, base);
    buffer[at++] = ',';
    at += decimal(buffer + at, size);
    buffer[at] = '\0';
    return buffer;
}

/* One field of a row against a string on the wire: equal lengths, equal
 * bytes. The field is NUL-terminated within its width. */
bool field_is(char const *field, uint32_t field_bytes, char const *text,
              uint32_t length) noexcept
{
    uint32_t held = 0;
    while (held < field_bytes && field[held] != '\0') {
        ++held;
    }
    if (held != length) {
        return false;
    }
    for (uint32_t i = 0; i < length; ++i) {
        if (field[i] != text[i]) {
            return false;
        }
    }
    return true;
}

/* The one method: both halves of the credential, or a refusal that says
 * nothing about which half failed. Answers first and returns the matched
 * row -- the session is started after the answer, because the caller's
 * answer must not wait on a spawn (specs/auth.md). -1 is every refusal. */
int answer_login(aegir::ipc::Owner &port, uint64_t const *words, uint32_t count) noexcept
{
    char const *name = nullptr;
    uint32_t name_length = 0;
    if (count == 0 ||
        !aegir::nmspace::unpack_string(words, count, aegir::authdb::kNameBytes, &name,
                                       &name_length)) {
        port.reply(0);
        return -1;
    }
    uint32_t const name_words = 1 + (name_length + 7) / 8;
    char const *secret = nullptr;
    uint32_t secret_length = 0;
    if (count < name_words ||
        !aegir::nmspace::unpack_string(words + name_words, count - name_words,
                                       aegir::authdb::kSecretBytes, &secret,
                                       &secret_length)) {
        port.reply(0);
        return -1;
    }
    for (uint32_t u = 0; u < g_users; ++u) {
        if (field_is(g_rows[u].name, aegir::authdb::kNameBytes, name, name_length) &&
            field_is(g_rows[u].secret, aegir::authdb::kSecretBytes, secret,
                     secret_length)) {
            port.reply(1);
            return static_cast<int>(u);
        }
    }
    port.reply(0);
    return -1;
}

/* A row field's length: NUL-terminated within its width. */
uint32_t field_length(char const *field, uint32_t field_bytes) noexcept
{
    uint32_t held = 0;
    while (held < field_bytes && field[held] != '\0') {
        ++held;
    }
    return held;
}

/* A directory the user owns (specs/ownership.md): mkdir, which makes every
 * missing component on the way (the mmd shape), then Owner and Protect 0700,
 * so the directory is the user's and only the user may write in it. The home
 * and the home's environment archive are both made this way. */
bool make_owned_dir(aegir::ipc::Consumer const &volume, char const *path,
                    uint32_t path_length, uint32_t user) noexcept
{
    uint64_t mout[aegir::nmspace::kPathMax / 8 + 1];
    uint32_t const mout_words = aegir::nmspace::pack_string(
        mout, path, path_length, aegir::nmspace::kPathMax);
    uint64_t min[1];
    aegir::ipc::WordsReply const made = volume.call_words(
        aegir::volume::kMethodMkdir, mout, mout_words, min, 1);
    if (made.error != 0 || made.count != 1 || min[0] != 1) {
        return false;
    }
    uint64_t owords[aegir::nmspace::kPathMax / 8 + 3];
    uint32_t ow = aegir::nmspace::pack_string(
        owords, path, path_length, aegir::nmspace::kPathMax);
    owords[ow++] = user;
    owords[ow++] = user;
    uint64_t oin[1];
    aegir::ipc::WordsReply const owned = volume.call_words(
        aegir::metadata::kMethodOwner, owords, ow, oin, 1);
    uint64_t pwords[aegir::nmspace::kPathMax / 8 + 2];
    uint32_t pw = aegir::nmspace::pack_string(
        pwords, path, path_length, aegir::nmspace::kPathMax);
    pwords[pw++] = 0700;
    uint64_t pin[1];
    aegir::ipc::WordsReply const protected_ = volume.call_words(
        aegir::metadata::kMethodProtect, pwords, pw, pin, 1);
    return owned.error == 0 && oin[0] == aegir::metadata::kOk &&
           protected_.error == 0 && pin[0] == aegir::metadata::kOk;
}

/* A subdirectory under the home, made and owned: the tail is appended to the
 * home's volume-relative path and make_owned_dir does the rest. The home's
 * environment archive and its script directory are both made this way. The
 * length is checked rather than assumed: the home path and the tail are each
 * within kPathMax, their sum need not be. */
bool make_owned_home_subdir(aegir::ipc::Consumer const &volume,
                            char const *home_rest, uint32_t home_rest_length,
                            char const *tail, uint32_t tail_length,
                            uint32_t user) noexcept
{
    if (home_rest_length + tail_length > aegir::nmspace::kPathMax) {
        return false;
    }
    char path[aegir::nmspace::kPathMax];
    uint32_t length = 0;
    for (uint32_t i = 0; i < home_rest_length; ++i) {
        path[length++] = home_rest[i];
    }
    for (uint32_t i = 0; i < tail_length; ++i) {
        path[length++] = tail[i];
    }
    return make_owned_dir(volume, path, length, user);
}

/* A per-badge namespace alias (specs/vfs.md): the badge, the name and the
 * path, replacing whatever that badge held. False when the namespace refuses. */
bool bind_name(uint64_t badge, char const *name, uint32_t name_length, char const *path,
               uint32_t path_length) noexcept
{
    uint64_t out[2 + aegir::nmspace::kNameMax / 8 + 1 +
                 aegir::nmspace::kPathMax / 8 + 1];
    uint64_t in[1];
    out[0] = badge;
    out[1] = 0; /* replace */
    uint32_t words = 2;
    words += aegir::nmspace::pack_string(out + words, name, name_length,
                                         aegir::nmspace::kNameMax);
    words += aegir::nmspace::pack_string(out + words, path, path_length,
                                         aegir::nmspace::kPathMax);
    aegir::ipc::WordsReply const bound =
        g_nmspace.call_words(aegir::nmspace::kMethodBind, out, words, in, 1);
    return bound.error == 0 && bound.count == 1 && in[0] == 1;
}

/* The home arc (specs/auth.md's Homes), run after the answer and before
 * the spawn: the row's home path is ensured -- one mkdir, the mmd shape --
 * and the session's badge is bound to Home:. The order is the point: the
 * session never sees a Home: that does not resolve. A home that will not
 * make is logged and the bind happens anyway -- the failure surfaces where
 * it belongs, at the session's first write into it. */
void ensure_home(uint32_t user, uint64_t badge) noexcept
{
    uint32_t const home_length =
        field_length(g_rows[user].home, aegir::authdb::kHomeBytes);
    if (home_length == 0 || g_home_slot == 0) {
        return;
    }
    char const *home = g_rows[user].home;

    bool made = false;
    {
        uint64_t out[aegir::nmspace::kPathMax / 8 + 1];
        uint32_t const out_words = aegir::nmspace::pack_string(
            out, home, home_length, aegir::nmspace::kPathMax);
        uint64_t in[aegir::nmspace::kResolveWords];
        bool cap_arrived = false;
        aegir::ipc::WordsReply const resolved = g_nmspace.call_transfer(
            aegir::nmspace::kMethodResolve, out, out_words, 0, in,
            aegir::nmspace::kResolveWords, &cap_arrived);
        char const *rest = nullptr;
        uint32_t rest_length = 0;
        if (resolved.error == 0 && cap_arrived &&
            aegir::nmspace::unpack_string(in, resolved.count, aegir::nmspace::kPathMax,
                                          &rest, &rest_length) &&
            aegir::ipc::take_received_cap(g_home_slot)) {
            aegir::ipc::Consumer const volume(g_home_slot);
            /* The directory belongs to the user and to no one else
             * (specs/ownership.md). The view is the namespace's half; without
             * the ownership it stays the system's and a second user reaching
             * it through Sys: would enter. */
            made = make_owned_dir(volume, rest, rest_length, user);
            if (!made) {
                write("      auth: FAIL the home would not be owned\n");
            }
            /* The home's environment archive (specs/environment.md): the
             * directory ENV: unions and a Set lands in. The same ownership,
             * so only the user may write in it. */
            if (made &&
                !make_owned_home_subdir(volume, rest, rest_length,
                                        "/Prefs/Env-Archive",
                                        sizeof("/Prefs/Env-Archive") - 1, user)) {
                write("      auth: FAIL the environment archive would not be made\n");
            }
            /* The home's script directory (specs/shell.md, specs/boot.md): the
             * user's S:, where a Shell-Startup override lives. The same
             * ownership, so only the user may write it. */
            if (made &&
                !make_owned_home_subdir(volume, rest, rest_length, "/S",
                                        sizeof("/S") - 1, user)) {
                write("      auth: FAIL the script directory would not be made\n");
            }
            seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_home_slot,
                              aegir::bootstrap::cnode_bits());
        }
    }
    if (!made) {
        write("      auth: the home would not be made -- the session starts "
              "without one\n");
    }

    /* The view over the home: named for the user, owned by the session's
     * badge, so Home: resolves to a private volume the session owns
     * (specs/ownership.md). A view that will not mount is logged and the raw
     * path is bound instead -- a home that is not its own is still a home. */
    char view_path[aegir::authdb::kHomeBytes + 1];
    uint32_t view_path_length = 0;
    {
        uint32_t const user_name_length =
            field_length(g_rows[user].name, aegir::authdb::kNameBytes);
        uint64_t mwords[2 * (aegir::authdb::kHomeBytes / 8 + 1) + 4];
        uint32_t w = 0;
        w += aegir::nmspace::pack_string(mwords + w, home, home_length,
                                         aegir::authdb::kHomeBytes);
        w += aegir::nmspace::pack_string(mwords + w, g_rows[user].name, user_name_length,
                                         aegir::nmspace::kNameMax);
        mwords[w++] = badge; /* the owner */
        mwords[w++] = 0;     /* flags */
        uint64_t mout[aegir::nmspace::kNameMax / 8 + 1];
        aegir::ipc::WordsReply const mounted = g_nmspace.call_words(
            aegir::nmspace::kMethodMount, mwords, w, mout,
            aegir::nmspace::kNameMax / 8 + 1);
        char const *got = nullptr;
        if (mounted.error == 0 && mounted.count > 0 &&
            aegir::nmspace::unpack_string(mout, mounted.count, aegir::nmspace::kNameMax,
                                          &got, &view_path_length)) {
            for (uint32_t i = 0; i < view_path_length; ++i) {
                view_path[i] = got[i];
            }
            view_path[view_path_length++] = ':';
        } else {
            write("      auth: FAIL the home view would not mount\n");
        }
    }
    if (view_path_length == 0) {
        for (uint32_t i = 0; i < home_length; ++i) {
            view_path[i] = home[i];
        }
        view_path_length = home_length;
    }

    uint64_t out[2 + aegir::nmspace::kNameMax / 8 + 1 + aegir::nmspace::kPathMax / 8 + 1];
    out[0] = badge;
    out[1] = 0; /* replace: a session's Home: is its own single-member alias */
    uint32_t out_words = 2;
    out_words += aegir::nmspace::pack_string(out + out_words, "Home", 4,
                                             aegir::nmspace::kNameMax);
    out_words += aegir::nmspace::pack_string(out + out_words, view_path, view_path_length,
                                             aegir::nmspace::kPathMax);
    uint64_t in[1];
    aegir::ipc::WordsReply const bound =
        g_nmspace.call_words(aegir::nmspace::kMethodBind, out, out_words, in, 1);
    if (bound.error != 0 || bound.count != 1 || in[0] != 1) {
        write("      auth: FAIL the Home: bind was refused\n");
    }

    /* ENV: (specs/environment.md): the union of the user's archive -- first,
     * so its settings override, and the create target -- and the system's
     * base. Bound after Home:, because its first member is a Home: path. */
    static char const kEnvHome[] = "Home:Prefs/Env-Archive";
    static char const kEnvSys[] = "Sys:Prefs/Env-Archive";
    uint64_t env_out[2 + aegir::nmspace::kNameMax / 8 + 1 + aegir::nmspace::kPathMax / 8 + 1];
    uint64_t env_in[1];
    env_out[0] = badge;
    env_out[1] = aegir::nmspace::kBindCreate;
    uint32_t env_words = 2;
    env_words += aegir::nmspace::pack_string(env_out + env_words, "ENV", 3,
                                             aegir::nmspace::kNameMax);
    env_words += aegir::nmspace::pack_string(env_out + env_words, kEnvHome,
                                             sizeof(kEnvHome) - 1,
                                             aegir::nmspace::kPathMax);
    aegir::ipc::WordsReply const env_first = g_nmspace.call_words(
        aegir::nmspace::kMethodBind, env_out, env_words, env_in, 1);
    env_out[1] = aegir::nmspace::kBindAppend;
    env_words = 2;
    env_words += aegir::nmspace::pack_string(env_out + env_words, "ENV", 3,
                                             aegir::nmspace::kNameMax);
    env_words += aegir::nmspace::pack_string(env_out + env_words, kEnvSys,
                                             sizeof(kEnvSys) - 1,
                                             aegir::nmspace::kPathMax);
    aegir::ipc::WordsReply const env_second = g_nmspace.call_words(
        aegir::nmspace::kMethodBind, env_out, env_words, env_in, 1);
    if (env_first.error != 0 || env_first.count != 1 || env_in[0] != 1 ||
        env_second.error != 0 || env_second.count != 1 || env_in[0] != 1) {
        write("      auth: FAIL the ENV: bind was refused\n");
    }

    /* S: (specs/shell.md, specs/boot.md): the session's script directory,
     * where the shell looks for Shell-Startup. A single-member alias of the
     * user's Home:S, made and owned above, so a user's override is a file the
     * user owns. The system's Sys:S is a separate name, not unioned: the
     * system's scripts never appear in the user's S:, and editing them needs
     * elevation. */
    static char const kScriptHome[] = "Home:S";
    uint64_t s_out[2 + aegir::nmspace::kNameMax / 8 + 1 + aegir::nmspace::kPathMax / 8 + 1];
    uint64_t s_in[1];
    s_out[0] = badge;
    s_out[1] = 0; /* replace */
    uint32_t s_words = 2;
    s_words += aegir::nmspace::pack_string(s_out + s_words, "S", 1,
                                           aegir::nmspace::kNameMax);
    s_words += aegir::nmspace::pack_string(s_out + s_words, kScriptHome,
                                           sizeof(kScriptHome) - 1,
                                           aegir::nmspace::kPathMax);
    aegir::ipc::WordsReply const s_bound = g_nmspace.call_words(
        aegir::nmspace::kMethodBind, s_out, s_words, s_in, 1);
    if (s_bound.error != 0 || s_bound.count != 1 || s_in[0] != 1) {
        write("      auth: FAIL the S: bind was refused\n");
    }

    /* C: (specs/dos.md): the command directory. A single-member alias of
     * Sys:C for now -- a session's own Home:C, and the command that installs
     * into it, arrive with a later slice. Read-only here: no create flag,
     * because a user does not write the system's command directory. */
    static char const kCmdSys[] = "Sys:C";
    uint64_t c_out[2 + aegir::nmspace::kNameMax / 8 + 1 + aegir::nmspace::kPathMax / 8 + 1];
    uint64_t c_in[1];
    c_out[0] = badge;
    c_out[1] = 0; /* replace */
    uint32_t c_words = 2;
    c_words += aegir::nmspace::pack_string(c_out + c_words, "C", 1,
                                           aegir::nmspace::kNameMax);
    c_words += aegir::nmspace::pack_string(c_out + c_words, kCmdSys,
                                           sizeof(kCmdSys) - 1, aegir::nmspace::kPathMax);
    aegir::ipc::WordsReply const c_bound =
        g_nmspace.call_words(aegir::nmspace::kMethodBind, c_out, c_words, c_in, 1);
    if (c_bound.error != 0 || c_bound.count != 1 || c_in[0] != 1) {
        write("      auth: FAIL the C: bind was refused\n");
    }
}

/* The teardown, once the wait says the session is done (specs/auth.md's
 * Session reclaim): the badge's handles go first -- one reap per volume
 * the namespace names, walked through count/describe/resolve, because a
 * handle is a filesystem's row and not a kernel object -- then its
 * aliases, then the revoke that deletes everything the session was, and
 * the slot cursor returns to the login's mark. Runs on the spawn-failure
 * paths too: the Home: bind has already happened by then, and a (badge,
 * name) pair binds once (specs/vfs.md) -- left bound, the next login's
 * bind of the same serial would be refused. */
void reclaim_session(uint64_t badge, seL4_CPtr mark, uintptr_t scratch_mark,
                     aegir::mem::Account const &session_account) noexcept
{
    uint64_t reaped = 0;
    uint64_t in[1];
    aegir::ipc::WordsReply const counted =
        g_nmspace.call_words(aegir::nmspace::kMethodCount, nullptr, 0, in, 1);
    uint64_t const volumes = (counted.error == 0 && counted.count == 1) ? in[0] : 0;
    for (uint64_t v = 0; v < volumes; ++v) {
        uint64_t row_words[aegir::nmspace::kRowWords];
        aegir::ipc::WordsReply const described =
            g_nmspace.call_words(aegir::nmspace::kMethodDescribe, &v, 1, row_words,
                                 aegir::nmspace::kRowWords);
        if (described.error != 0 || described.count < aegir::nmspace::kRowWords) {
            continue;
        }
        auto const &row = *reinterpret_cast<aegir::nmspace::Row const *>(row_words);
        if (row.bound == 0) {
            continue;
        }
        /* A volume's name, resolved as "NAME:" with an empty rest: the
         * answer is the filesystem's port, minted with our badge. */
        char path[aegir::nmspace::kNameMax + 1];
        uint32_t name_length = 0;
        while (name_length < aegir::nmspace::kNameMax && row.name[name_length] != '\0') {
            path[name_length] = row.name[name_length];
            ++name_length;
        }
        path[name_length] = ':';
        uint64_t out[aegir::nmspace::kPathMax / 8 + 1];
        uint32_t const out_words = aegir::nmspace::pack_string(out, path, name_length + 1,
                                                               aegir::nmspace::kPathMax);
        uint64_t rin[aegir::nmspace::kResolveWords];
        bool cap_arrived = false;
        aegir::ipc::WordsReply const resolved =
            g_nmspace.call_transfer(aegir::nmspace::kMethodResolve, out, out_words, 0, rin,
                                    aegir::nmspace::kResolveWords, &cap_arrived);
        if (resolved.error != 0 || !cap_arrived ||
            !aegir::ipc::take_received_cap(g_home_slot)) {
            continue;
        }
        aegir::ipc::Consumer const volume(g_home_slot);
        uint64_t const badge_word = badge;
        uint64_t bin[1];
        aegir::ipc::WordsReply const answered =
            volume.call_words(aegir::volume::kMethodReap, &badge_word, 1, bin, 1);
        if (answered.error == 0 && answered.count == 1) {
            reaped += bin[0];
        }
        /* The resolve's cap is ours to dispose of: one slot, deleted after
         * each use, so a reclaim does not spend what the next one needs. */
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_home_slot,
                          aegir::bootstrap::cnode_bits());
    }
    uint64_t const badge_word = badge;
    uint64_t uin[1];
    aegir::ipc::WordsReply const unbound_reply =
        g_nmspace.call_words(aegir::nmspace::kMethodUnbind, &badge_word, 1, uin, 1);
    uint64_t const unbound =
        (unbound_reply.error == 0 && unbound_reply.count == 1) ? uin[0] : 0;

    /* The memory's way back is the memory service's: one release per badge the
     * session used revokes every chunk that badge owns -- the session's
     * objects, its spawn's staging, the bureau's and terminal's runtime
     * untypeds, all retyped from those chunks -- and with them go the minted
     * port copies in their CSpaces (specs/authority.md's retained-copy path,
     * specs/memory.md). The terminal's own badge is released too, so a nested
     * terminal's memory, charged there, comes back. The kernel unmaps a mapped
     * frame when the cap goes (finaliseCap), so the staging's scratch-window
     * pages are already unmapped; the window's cursor just needs to be told.
     * The slots past the mark are empty, so the cursor returns to it. */
    aegir::ipc::Consumer const mem(g_spawn_mem);
    uint64_t released = 0;
    uint64_t const session_owner = badge;
    (void)mem.call_words(aegir::memory::kMethodRelease, &session_owner, 1, &released, 1);
    uint64_t const terminal_owner = badge + 1;
    (void)mem.call_words(aegir::memory::kMethodRelease, &terminal_owner, 1, &released, 1);
    g_scratch.rewind(scratch_mark);
    g_objects.slot_release(mark);
    write("      auth: session reclaimed: ");
    aegir::debug_write_unsigned(reaped);
    write(reaped == 1 ? " handle, " : " handles, ");
    aegir::debug_write_unsigned(unbound);
    write(unbound == 1 ? " alias, " : " aliases, ");
    aegir::debug_write_unsigned(session_account.bytes / 1024);
    write(" KiB charged back to the pool\n");
}

/* A successful login starts a session (specs/auth.md): the smoke over the
 * serial line, the bureau when the caller is the greeter (specs/console.md's
 * login arc). The badge is the user class bit, the row as the user id, and
 * the serial counting what the user has run (specs/authority.md); the ports
 * are the delegatable copies badged with it. Everything the spawn puts down
 * -- the session's objects and the spawn's own staging -- is retyped from
 * the session pool and slotted past the mark, so the teardown after the
 * wait takes it all back; the bureau's mapping kit (an untyped for its page
 * tables, its own VSpace root) is carved from the pool too. What the
 * reclaim does NOT take is the bureau's slice: the screen is the session's
 * visible remainder, and a console reap of the session badge is the
 * re-login arc's to make, not today's. Then the wait for its ready, and
 * serving resumes (specs/auth.md's Session reclaim). */
void start_session(uint32_t user, bool bureau) noexcept
{
    uint32_t const serial = g_serials[user];
    uint64_t const badge = aegir::ipc::make_user_badge(user, serial);
    /* The terminal is a second session child with a badge -- and so a console
     * slice -- of its own: the console carves one slice per badge and refuses
     * a second attach, so two windows cannot share one (specs/console.md).
     * Its serial is the next. */
    uint64_t const terminal_badge = aegir::ipc::make_user_badge(user, serial + 1);
    /* The session's launcher (specs/launch.md) is a third: it holds the spawn
     * kit and serves launch.session. Its serial is the next. */
    uint64_t const launcher_badge = aegir::ipc::make_user_badge(user, serial + 2);
    /* The home first: ensured and bound before the spawn, so the session
     * never sees a Home: that does not resolve (specs/auth.md's Homes). */
    ensure_home(user, badge);
    if (bureau) {
        ensure_home(user, terminal_badge);
    }

    seL4_CPtr const mark = g_objects.slot_mark();
    /* The scratch window's own mark: the spawn stages the child's block and
     * stack through it, and those pages are the session's to hand back --
     * the rewind in reclaim_session is what keeps one login's staging from
     * climbing the window until a table allocation collides with the
     * session's own slots. */
    uintptr_t const scratch_mark = g_scratch.next();
    aegir::mem::Account session_account{"session", 0, 0, 0};
    if (g_spawn_mem == 0) {
        write("      auth: FAIL no mem.main to build the session from\n");
        reclaim_session(badge, mark, scratch_mark, session_account);
        return;
    }
    /* The session's mem.main copy: minted fresh with this login's badge, so
     * the memory service charges the session's every chunk to it and one
     * release takes them all back. */
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_session_mem_call,
                      aegir::bootstrap::cnode_bits());
    if (seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, g_session_mem_call,
                        aegir::bootstrap::cnode_bits(), aegir::bootstrap::kSlotOwnCNode,
                        g_spawn_mem, aegir::bootstrap::cnode_bits(),
                        seL4_CapRights_new(1, 1, 0, 1), badge) != seL4_NoError) {
        write("      auth: FAIL the session's mem.main copy would not mint\n");
        reclaim_session(badge, mark, scratch_mark, session_account);
        return;
    }
    g_session_mem.adopt_nodes(g_session_nodes, sizeof(g_session_nodes));
    g_session_mem.reset();
    /* One cursor up, one down (specs/direction.md): the session pool takes its
     * slots from the top of auth's CSpace, auth's own objects from the bottom,
     * so a slot cannot be handed out twice while anything is left. A pool in a
     * CNode of its own was the first choice and cannot work -- a capability
     * inside a nested CNode cannot be invoked, because the root's guard covers
     * every bit its CNode does not index, so the CPtr walk always ends in the
     * root (kernel/src/kernel/cspace.c:51, :126-192). */
    g_session_mem.adopt_slots_down(g_slots_first, g_slots_end - g_slots_first, 0);
    g_session_mem.set_untyped_source(session_untyped_source, &g_session_mem);
    aegir::mem::Arena session_arena(g_session_mem, g_scratch, session_account);

    seL4_Error fault_error = seL4_NoError;
    seL4_CPtr const fault = g_session_mem.alloc_object(seL4_EndpointObject, seL4_EndpointBits,
                                                       session_account, &fault_error);
    if (fault == 0) {
        write("      auth: FAIL no fault endpoint for the session\n");
        reclaim_session(badge, mark, scratch_mark, session_account);
        return;
    }
    /* The launch.session endpoint (specs/launch.md): auth makes it and holds
     * the halves -- the owner to the session's launcher, a caller to the bureau
     * and the terminal -- so no client depends on a name the launcher chose. */
    seL4_CPtr launch_port = 0;
    if (bureau) {
        seL4_Error port_error = seL4_NoError;
        launch_port = g_session_mem.alloc_object(seL4_EndpointObject, seL4_EndpointBits,
                                                 session_account, &port_error);
        if (launch_port == 0) {
            write("      auth: FAIL no launch endpoint for the session\n");
            reclaim_session(badge, mark, scratch_mark, session_account);
            return;
        }
    }
    /* The bureau's kit: 1 MiB of the pool -- the page tables its slice mapping
     * is retyped from, and the heap the toolkit (its screen bar and font)
     * allocates from, where the raw backdrop it replaced allocated nothing.
     * The grant travels with its size, because a service cannot ask the
     * kernel how large an untyped is. */
    constexpr uint32_t kBureauUntypedBits = 20;
    uint64_t bureau_untyped_physical = 0;
    seL4_CPtr bureau_untyped = 0;
    if (bureau) {
        seL4_Error untyped_error = seL4_NoError;
        bureau_untyped = g_session_mem.carve_untyped(kBureauUntypedBits,
                                                     session_account, &untyped_error,
                                                     &bureau_untyped_physical);
        if (bureau_untyped == 0) {
            write("      auth: FAIL no untyped for the bureau's tables\n");
            reclaim_session(badge, mark, scratch_mark, session_account);
            return;
        }
    }
    aegir::spawn::PortGrant ports[6] = {
        {aegir::log::kPortName, aegir::log::kPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared, g_spawn_log, seL4_CapRights_new(1, 0, 0, 1),
         badge, 0},
        /* The namespace, with Grant: a resolve's answer carries a
         * capability, and a cap crosses only between halves that may grant
         * (specs/services.md). */
        {aegir::nmspace::kPortName, aegir::nmspace::kPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared + 1, g_spawn_nmspace,
         seL4_CapRights_new(1, 1, 0, 1), badge, 0},
        /* The bureau's extras (dead entries for the smoke -- the count says
         * which are live): the console, with Grant, because frame and listen
         * answers carry capabilities; and the untyped, whole. */
        {aegir::console::kPortName, aegir::console::kPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared + 2, g_spawn_gui,
         seL4_CapRights_new(1, 1, 0, 1), badge, 0},
        {"untyped", 7, aegir::bootstrap::kSlotFirstDeclared + 3, bureau_untyped,
         seL4_AllRights, 0, kBureauUntypedBits},
        /* A caller half of the session's launcher (specs/launch.md): the
         * bureau's Execute launches through it. auth made the endpoint above. */
        {aegir::launch::kPortName, aegir::launch::kPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared + 4, launch_port,
         seL4_CapRights_new(1, 1, 0, 1), badge, 0},
        /* The bureau.menu owner half (specs/workbench.md): the bureau reads it
         * -- it receives the port's calls -- and it is unbadged, because a
         * receiver's badge never identifies it. A boot whose director made no
         * such port passes it with a null cap, which the spawner would refuse;
         * the count below keeps that entry out when there is none. */
        {"bureau.menu", 11, aegir::bootstrap::kSlotFirstDeclared + 5,
         g_spawn_bureau_menu, seL4_CanRead, 0, 0},
    };
    static char const kSessionName[] = "session.smoke";
    static char const kSessionBinary[] = "aegir-session-smoke";
    static char const kBureauName[] = "session.bureau";
    static char const kBureauBinary[] = "aegir-bureau";
    aegir::spawn::Request request{};
    if (bureau) {
        request.name = kBureauName;
        request.name_length = sizeof(kBureauName) - 1;
        request.binary = kBureauBinary;
        request.binary_length = sizeof(kBureauBinary) - 1;
        request.give_vspace = true;
        request.untyped_physical = bureau_untyped_physical;
        request.untyped_bits = kBureauUntypedBits;
    } else {
        request.name = kSessionName;
        request.name_length = sizeof(kSessionName) - 1;
        request.binary = kSessionBinary;
        request.binary_length = sizeof(kSessionBinary) - 1;
    }
    request.account = g_rows[user].account;
    request.account_length = field_length(g_rows[user].account, aegir::authdb::kAccountBytes);
    /* A session stands on its own Home (specs/environment.md): the alias auth
     * already bound for it below, so a relative path resolves into the user's
     * tree rather than the system's. */
    static char const kHomeCwd[] = "Home:";
    request.cwd = kHomeCwd;
    request.cwd_length = sizeof(kHomeCwd) - 1;
    request.priority = seL4_MaxPrio - 2;
    request.ports = ports;
    request.port_count = bureau ? (5 + (g_spawn_bureau_menu != 0 ? 1 : 0)) : 2;
    request.fault_endpoint = fault;
    request.badge = badge;

    /* The spawner is the session's own: over the pool and the slots past
     * the mark, so nothing it puts down outlives the reclaim. */
    aegir::spawn::Initrd const initrd(reinterpret_cast<void const *>(g_binaries_address),
                                      g_binaries_bytes);
    aegir::spawn::Spawner spawner(g_session_mem, g_scratch, session_arena, initrd,
                                  g_asid_pool,
                                  static_cast<seL4_CPtr>(aegir::bootstrap::kSlotOwnCNode),
                                  aegir::bootstrap::cnode_bits());
    aegir::spawn::Process process{};
    if (!spawner.spawn(request, session_account, process)) {
        write("      auth: FAIL spawning the session: ");
        write(spawner.problem());
        char const *const detail = spawner.detail();
        if (detail != nullptr && detail[0] != '\0') {
            write(" (");
            write(detail);
            write(")");
        }
        write("\n");
        reclaim_session(badge, mark, scratch_mark, session_account);
        return;
    }
    write("      auth: ");
    write(g_rows[user].name, field_length(g_rows[user].name, aegir::authdb::kNameBytes));
    write(" authenticated, session started, badge ");
    aegir::debug_write_hex(badge);
    write("\n");

    /* The terminal (specs/terminal.md, specs/shell.md): a second session
     * child, with its own badge -- and so its own console slice -- the
     * namespace, and the user's Home as its current directory. Spawned after
     * the bureau, so its window is created above the backdrop. It is the same
     * spawner over the same pool, and its failure is logged without taking
     * the bureau down. */
    if (bureau) {
        seL4_Error terminal_fault_error = seL4_NoError;
        seL4_CPtr const terminal_fault =
            g_session_mem.alloc_object(seL4_EndpointObject, seL4_EndpointBits,
                                       session_account, &terminal_fault_error);
        seL4_Error terminal_untyped_error = seL4_NoError;
        uint64_t terminal_untyped_physical = 0;
        /* The terminal's own memory: its toolkit (fonts, the slice's page
         * tables) and the heap a command's image is read into before the
         * spawner copies it. A hosted command is near a megabyte, so the
         * bureau's 1 MiB is not enough for the terminal. */
        constexpr uint32_t kTerminalUntypedBits = 22;
        /* Its CSpace (specs/authority.md): larger than a service's 4096 slots,
         * because it is a launcher -- staging a nested terminal's near-megabyte
         * image and mapping its own 8 MiB heap compete for CSpace slots in a
         * 4096 that a plain service gets. 8192 holds both. */
        constexpr uint32_t kTerminalCNodeBits = 13;
        seL4_CPtr const terminal_untyped =
            g_session_mem.carve_untyped(kTerminalUntypedBits, session_account,
                                        &terminal_untyped_error,
                                        &terminal_untyped_physical);
        /* The shell pool: the shell is its own process now, and its runtime
         * untyped is carved once from here and never reclaimed (specs/shell.md).
         * A command's memory no longer comes from a pool: the terminal asks the
         * memory service for a chunk owned by the command's own badge
         * (specs/memory.md Phase 3). */
        constexpr uint32_t kTerminalShellPoolBits = 22;
        seL4_Error shell_pool_error = seL4_NoError;
        uint64_t shell_pool_physical = 0;
        seL4_CPtr const terminal_shell_pool =
            g_session_mem.carve_untyped(kTerminalShellPoolBits, session_account,
                                        &shell_pool_error, &shell_pool_physical);
        /* The terminal's kit, from the one first-class grant (specs/launch.md):
         * its own console and identity, its runtime, and the unbadged sources
         * it hands its commands and nested terminals. The namespace copy is
         * badged for the session, so the terminal and its shell resolve Home:
         * and ENV:. */
        if (!mint_session_nmspace(g_kit_nmspace_slot, terminal_badge)) {
            write("      auth: FAIL no namespace copy for the terminal\n");
            reclaim_session(badge, mark, scratch_mark, session_account);
            return;
        }
        aegir::spawn::Kit terminal_kit{};
        terminal_kit.log = g_spawn_log;
        terminal_kit.console_gui = g_spawn_gui;
        terminal_kit.mem_main = g_spawn_mem;
        terminal_kit.asid_pool = g_asid_pool;
        terminal_kit.clock = g_spawn_clock;
        terminal_kit.timer = g_spawn_timer;
        terminal_kit.nmspace = g_kit_nmspace_slot;
        aegir::spawn::Child terminal_child{};
        terminal_child.badge = terminal_badge;
        terminal_child.runtime = terminal_untyped;
        terminal_child.runtime_bits = kTerminalUntypedBits;
        terminal_child.shell_pool = terminal_shell_pool;
        terminal_child.shell_pool_bits = kTerminalShellPoolBits;
        terminal_child.launcher = true;
        aegir::spawn::PortGrant terminal_ports[16];
        uint32_t terminal_port_count =
            aegir::spawn::launcher_ports(terminal_kit, terminal_child, terminal_ports, 16);
        /* The launcher's caller half (specs/launch.md): the terminal relays the
         * shell's command lines to it, so a command is started by the service
         * that holds the kit, not by the terminal. */
        terminal_ports[terminal_port_count] = {
            aegir::launch::kPortName, aegir::launch::kPortNameLength,
            aegir::bootstrap::kSlotFirstDeclared + terminal_port_count, launch_port,
            seL4_CapRights_new(1, 1, 0, 1), terminal_badge, 0};
        ++terminal_port_count;
        static char const kTerminalName[] = "session.terminal";
        static char const kTerminalBinary[] = "aegir-terminal";
        aegir::spawn::Request terminal_request{};
        terminal_request.name = kTerminalName;
        terminal_request.name_length = sizeof(kTerminalName) - 1;
        terminal_request.binary = kTerminalBinary;
        terminal_request.binary_length = sizeof(kTerminalBinary) - 1;
        terminal_request.account = g_rows[user].account;
        terminal_request.account_length =
            field_length(g_rows[user].account, aegir::authdb::kAccountBytes);
        terminal_request.cwd = kHomeCwd;
        terminal_request.cwd_length = sizeof(kHomeCwd) - 1;
        terminal_request.priority = seL4_MaxPrio - 2;
        terminal_request.ports = terminal_ports;
        terminal_request.port_count = terminal_port_count;
        terminal_request.fault_endpoint = terminal_fault;
        terminal_request.badge = terminal_badge;
        terminal_request.cnode_bits = kTerminalCNodeBits;
        terminal_request.give_vspace = true;
        terminal_request.untyped_physical = terminal_untyped_physical;
        terminal_request.untyped_bits = kTerminalUntypedBits;
        /* The badge range the terminal hands out (specs/launch.md): its
         * commands and any nested terminal draw serials from it, so no two of
         * a session's processes share one. Serial + 3 skips the session's own
         * badge, this terminal's and the launcher's. It moves to the launcher
         * when the terminal relays its launches to it. */
        char const *const terminal_badge_range =
            badge_range_env(serial + 3, aegir::ipc::kSessionSerialStride - 3);
        char const *const terminal_environment[1] = {terminal_badge_range};
        terminal_request.environment = terminal_environment;
        terminal_request.environment_count = 1;
        aegir::spawn::Process terminal_process{};
        if (terminal_fault == 0 || terminal_untyped == 0 ||
            !spawner.spawn(terminal_request, session_account, terminal_process)) {
            write("      auth: FAIL spawning the terminal: ");
            write(spawner.problem());
            char const *const detail = spawner.detail();
            if (detail != nullptr && detail[0] != '\0') {
                write(" (");
                write(detail);
                write(")");
            }
            write("\n");
        } else {
            write("      auth: the terminal is up\n");
        }
        /* The kit's namespace copy was auth's scratch: the terminal holds its
         * own now (specs/launch.md). */
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_kit_nmspace_slot,
                          aegir::bootstrap::cnode_bits());

        /* The session's launcher (specs/launch.md): one per session, holding
         * the spawn kit and serving launch.session, whose endpoint auth made
         * above. It draws nothing -- its runtime is for its own objects and
         * the staging -- and it draws a nested shell's pool from mem.main on
         * demand, so it is given no shell pool. Its namespace is badged for the
         * session, so it and its children resolve every assign and volume the
         * session has, whatever the session is. */
        seL4_Error launcher_fault_error = seL4_NoError;
        seL4_CPtr const launcher_fault = g_session_mem.alloc_object(
            seL4_EndpointObject, seL4_EndpointBits, session_account, &launcher_fault_error);
        constexpr uint32_t kLauncherUntypedBits = 22;
        seL4_Error launcher_untyped_error = seL4_NoError;
        uint64_t launcher_untyped_physical = 0;
        seL4_CPtr const launcher_untyped = g_session_mem.carve_untyped(
            kLauncherUntypedBits, session_account, &launcher_untyped_error,
            &launcher_untyped_physical);
        if (launcher_fault == 0 || launcher_untyped == 0) {
            write("      auth: FAIL no memory for the launcher\n");
        } else if (!mint_session_nmspace(g_kit_nmspace_slot, badge)) {
            write("      auth: FAIL no namespace for the launcher\n");
        } else {
            aegir::spawn::Kit launcher_kit{};
            launcher_kit.log = g_spawn_log;
            launcher_kit.console_gui = g_spawn_gui;
            launcher_kit.mem_main = g_spawn_mem;
            launcher_kit.asid_pool = g_asid_pool;
            launcher_kit.clock = g_spawn_clock;
            launcher_kit.timer = g_spawn_timer;
            launcher_kit.nmspace = g_kit_nmspace_slot;
            aegir::spawn::Child launcher_child{};
            launcher_child.badge = launcher_badge;
            launcher_child.runtime = launcher_untyped;
            launcher_child.runtime_bits = kLauncherUntypedBits;
            launcher_child.launcher = true;
            aegir::spawn::PortGrant launcher_ports[16];
            uint32_t launcher_port_count = aegir::spawn::launcher_ports(
                launcher_kit, launcher_child, launcher_ports, 16);
            /* The endpoint's owner half: the launcher serves on it. An owner
             * needs Read to receive; the caller's halves carry Write instead,
             * which is the other side of the same endpoint. */
            launcher_ports[launcher_port_count] = {
                aegir::launch::kPortName, aegir::launch::kPortNameLength,
                aegir::bootstrap::kSlotFirstDeclared + launcher_port_count, launch_port,
                seL4_CapRights_new(0, 0, 1, 0), 0, 0};
            ++launcher_port_count;
            /* The launcher's own caller half (specs/launch.md): with it the
             * launcher hands a nested terminal the same caller half a shell is
             * given, so a nested terminal's commands go through the one
             * launcher too. */
            launcher_ports[launcher_port_count] = {
                "spawn:launch.session", 20,
                aegir::bootstrap::kSlotFirstDeclared + launcher_port_count, launch_port,
                seL4_CapRights_new(1, 1, 0, 1), launcher_badge, 0};
            ++launcher_port_count;
            /* The launcher's own badged mem.main (specs/memory.md): its heap
             * grows through it, as a command's does, so the session's spawning
             * does not exhaust the fixed seed the launcher was started with
             * (its images are megabytes, and it is the one that stages them). */
            launcher_ports[launcher_port_count] = {
                aegir::memory::kPortName, aegir::memory::kPortNameLength,
                aegir::bootstrap::kSlotFirstDeclared + launcher_port_count, g_spawn_mem,
                seL4_CapRights_new(1, 1, 0, 1), launcher_badge, 0};
            ++launcher_port_count;
            static char const kLauncherName[] = "session.launcher";
            static char const kLauncherBinary[] = "aegir-launcher";
            aegir::spawn::Request launcher_request{};
            launcher_request.name = kLauncherName;
            launcher_request.name_length = sizeof(kLauncherName) - 1;
            launcher_request.binary = kLauncherBinary;
            launcher_request.binary_length = sizeof(kLauncherBinary) - 1;
            launcher_request.account = g_rows[user].account;
            launcher_request.account_length =
                field_length(g_rows[user].account, aegir::authdb::kAccountBytes);
            launcher_request.cwd = kHomeCwd;
            launcher_request.cwd_length = sizeof(kHomeCwd) - 1;
            /* The badge range the launcher hands out (specs/launch.md): its
             * commands and any nested terminal draw serials from it, so no two
             * of a session's processes share one. Serial + 3 skips the
             * session's own badge, the terminal's and the launcher's. */
            char const *const launcher_badge_range =
                badge_range_env(serial + 3, aegir::ipc::kSessionSerialStride - 3);
            char const *const launcher_environment[1] = {launcher_badge_range};
            launcher_request.environment = launcher_environment;
            launcher_request.environment_count = 1;
            launcher_request.priority = seL4_MaxPrio - 2;
            launcher_request.ports = launcher_ports;
            launcher_request.port_count = launcher_port_count;
            launcher_request.fault_endpoint = launcher_fault;
            launcher_request.badge = launcher_badge;
            launcher_request.give_vspace = true;
            /* A launcher stages images of its own (a nested terminal's is near
             * a megabyte), so it gets the larger CSpace a spawner needs
             * (specs/authority.md), exactly as the terminal does. */
            launcher_request.cnode_bits = kTerminalCNodeBits;
            launcher_request.untyped_physical = launcher_untyped_physical;
            launcher_request.untyped_bits = kLauncherUntypedBits;
            aegir::spawn::Process launcher_process{};
            if (!spawner.spawn(launcher_request, session_account, launcher_process)) {
                write("      auth: FAIL spawning the launcher: ");
                write(spawner.problem());
                char const *const detail = spawner.detail();
                if (detail != nullptr && detail[0] != '\0') {
                    write(" (");
                    write(detail);
                    write(")");
                }
                write("\n");
            } else {
                write("      auth: the launcher is up\n");
            }
        }
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_kit_nmspace_slot,
                          aegir::bootstrap::cnode_bits());
    }
    g_serials[user] += aegir::ipc::kSessionSerialStride;

    /* The ready, waited on the way the partition manager waits for a
     * filesystem's: a session that faults first leaves us here, which is
     * what waiting on it is for. While sessions are short-lived the ready
     * is also the exit -- the smoke signals as its last act -- so the wait
     * returning is how we know the session died (specs/auth.md). */
    seL4_Wait(process.supervision, nullptr);
    write("      auth: session ready\n");
    reclaim_session(badge, mark, scratch_mark, session_account);
}

/* The boot session (specs/boot.md): the system's Startup-Sequence, run once
 * before the greeter, as a system badge. It is a terminal like a session's --
 * a window from console.gui, a shell from the shell pool -- but its memory
 * comes from auth's own delegation, not the session pool, because it is not
 * reclaimed: on failure it stays as the read-only view (the failure arc's
 * piece). auth receives the boot outcome -- the status the shell sends -- and
 * answers true when the greeter should run, false when the boot failed and the
 * failure view stands instead. */
bool start_boot_session(aegir::mem::Arena &arena) noexcept
{
    if (g_spawn_gui == 0 || g_spawn_nmspace == 0 || g_spawn_log == 0 ||
        g_spawn_mem == 0 || g_asid_pool == 0 || g_binaries_bytes == 0) {
        write("      auth: no kit for the boot session -- Startup-Sequence is skipped\n");
        return true;
    }
    aegir::mem::Account account{"boot", 0, 0, 0};
    seL4_Error error = seL4_NoError;
    seL4_CPtr const fault = g_objects.alloc_object(seL4_EndpointObject,
                                                   seL4_EndpointBits, account, &error);
    /* The boot outcome: an endpoint, not a notification, because the shell has
     * a word to say -- 0 when Startup-Sequence finished, nonzero when it failed
     * (specs/boot.md). auth receives it and decides whether the greeter runs. */
    seL4_CPtr const boot_status = g_objects.alloc_object(
        seL4_EndpointObject, seL4_EndpointBits, account, &error);
    constexpr uint32_t kTerminalUntypedBits = 22;
    uint64_t terminal_untyped_physical = 0;
    seL4_CPtr const terminal_untyped = g_objects.carve_untyped(
        kTerminalUntypedBits, account, &error, &terminal_untyped_physical);
    constexpr uint32_t kShellPoolBits = 22;
    uint64_t shell_pool_physical = 0;
    seL4_CPtr const shell_pool = g_objects.carve_untyped(
        kShellPoolBits, account, &error, &shell_pool_physical);
    if (fault == 0 || boot_status == 0 || terminal_untyped == 0 || shell_pool == 0) {
        write("      auth: FAIL no memory for the boot session\n");
        return false;
    }

    /* The boot session's command directory: C: -> Sys:C, the same alias a
     * session gets, so a command in Startup-Sequence resolves (specs/dos.md).
     * Sys: is the boot alias and NIL: is public, so neither needs binding.
     * Sys: is a filesystem's to register and comes up after auth, so the bind
     * is retried until the boot volume is there -- the same wait the user
     * database's resolve does above. */
    static char const kCmdSys[] = "Sys:C";
    for (;;) {
        if (bind_name(kBootBadge, "C", 1, kCmdSys, sizeof(kCmdSys) - 1)) {
            break;
        }
        seL4_Yield();
    }

    /* The boot terminal's kit, from the one first-class grant (specs/launch.md):
     * like a session's terminal, but badged with the boot badge and not a
     * launcher of launchers (it runs Startup-Sequence, no NEWSHELL). Its
     * namespace copies are badged for the boot session, so its shell resolves
     * Sys: and the boot aliases. */
    if (!mint_session_nmspace(g_kit_nmspace_slot, kBootBadge)) {
        write("      auth: FAIL no namespace copy for the boot terminal\n");
        return false;
    }
    aegir::spawn::Kit boot_kit{};
    boot_kit.log = g_spawn_log;
    boot_kit.console_gui = g_spawn_gui;
    boot_kit.mem_main = g_spawn_mem;
    boot_kit.asid_pool = g_asid_pool;
    boot_kit.clock = g_spawn_clock;
    boot_kit.timer = g_spawn_timer;
    boot_kit.nmspace = g_kit_nmspace_slot;
    aegir::spawn::Child boot_child{};
    boot_child.badge = kBootBadge;
    boot_child.runtime = terminal_untyped;
    boot_child.runtime_bits = kTerminalUntypedBits;
    boot_child.shell_pool = shell_pool;
    boot_child.shell_pool_bits = kShellPoolBits;
    boot_child.launcher = false;
    aegir::spawn::PortGrant ports[16];
    uint32_t port_count = aegir::spawn::launcher_ports(boot_kit, boot_child, ports, 16);
    /* The boot status endpoint is not part of the kit a launched program gets:
     * only the boot terminal's shell sends the outcome on it (specs/boot.md). */
    ports[port_count] = {"boot.status", 11,
                         aegir::bootstrap::kSlotFirstDeclared + port_count, boot_status,
                         seL4_AllRights, 0, 0};
    ++port_count;

    static char const kName[] = "system.boot";
    static char const kBinary[] = "aegir-terminal";
    static char const kAccount[] = "system";
    static char const kCwd[] = "Sys:";
    aegir::spawn::Request request{};
    request.name = kName;
    request.name_length = sizeof(kName) - 1;
    request.binary = kBinary;
    request.binary_length = sizeof(kBinary) - 1;
    request.account = kAccount;
    request.account_length = sizeof(kAccount) - 1;
    request.cwd = kCwd;
    request.cwd_length = sizeof(kCwd) - 1;
    /* The firmware's boot flags, for the shell the terminal starts: `aegir.fail`
     * makes the boot sequence's failure view come up even when the sequence
     * itself would succeed, which is how the failure path is exercised
     * (specs/boot.md). */
    static char const kEnvFail[] = "AEGIR_BOOTARGS=aegir.fail";
    static char const *const kBootEnvironment[] = {kEnvFail};
    uint32_t flags_length = 0;
    char const *const flags = aegir::bootstrap::boot_flags(&flags_length);
    bool force_fail = false;
    for (uint32_t i = 0; flags != nullptr && i + 10 <= flags_length && !force_fail; ++i) {
        static char const kFail[] = "aegir.fail";
        bool same = true;
        for (uint32_t j = 0; j < 10 && same; ++j) {
            if (flags[i + j] != kFail[j]) {
                same = false;
            }
        }
        force_fail = same;
    }
    if (force_fail) {
        request.environment = kBootEnvironment;
        request.environment_count = 1;
    }
    request.priority = seL4_MaxPrio - 2;
    request.ports = ports;
    request.port_count = port_count;
    request.fault_endpoint = fault;
    request.badge = kBootBadge;
    request.give_vspace = true;
    request.untyped_physical = terminal_untyped_physical;
    request.untyped_bits = kTerminalUntypedBits;

    aegir::spawn::Initrd const initrd(reinterpret_cast<void const *>(g_binaries_address),
                                      g_binaries_bytes);
    aegir::spawn::Spawner spawner(g_objects, g_scratch, arena, initrd, g_asid_pool,
                                  static_cast<seL4_CPtr>(aegir::bootstrap::kSlotOwnCNode),
                                  aegir::bootstrap::cnode_bits());
    aegir::spawn::Process process{};
    if (!spawner.spawn(request, account, process)) {
        write("      auth: FAIL spawning the boot session: ");
        write(spawner.problem());
        write("\n");
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_kit_nmspace_slot,
                          aegir::bootstrap::cnode_bits());
        return true;
    }
    /* The kit's namespace copy was auth's scratch: the boot terminal holds its
     * own now (specs/launch.md). */
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_kit_nmspace_slot,
                      aegir::bootstrap::cnode_bits());
    /* The script's end: the shell sends the outcome on `boot.status`
     * (specs/boot.md). 0 means Startup-Sequence finished -- its EndCLI closed
     * the boot window -- and the greeter runs; nonzero means it failed, the
     * shell has put the read-only view up, and no greeter does. */
    uint64_t badge = 0;
    seL4_Recv(boot_status, &badge);
    uint64_t const status = seL4_GetMR(0);
    if (status != 0) {
        write("      auth: the boot session failed -- the failure view stands\n");
        return false;
    }
    write("      auth: the boot session ran\n");
    return true;
}

/* The greeter (specs/console.md's login arc): auth's face, started once the
 * user database is read. Two things set it apart from a session. Its memory
 * comes from auth's own delegation, not the session pool: a login's reclaim
 * revokes the pool, and the greeter's windows must stand until the login it
 * brokers is done. And nobody waits on it: it runs beside the serving loop
 * until its login succeeds, the loop's reap of its badge takes the windows
 * and slice back, and the process itself is one-shot -- its few pages stay
 * spent, a boot's price, until the bureau arc owns re-login. The spawn's
 * staging through the scratch window ratchets the same way. */
void start_greeter(aegir::mem::Arena &arena) noexcept
{
    if (g_spawn_gui == 0 || g_spawn_login == 0 || !g_gui.valid()) {
        write("      auth: no kit for the greeter -- the screen stays dark\n");
        return;
    }
    aegir::mem::Account greeter_account{"greeter", 0, 0, 0};
    seL4_Error error = seL4_NoError;
    seL4_CPtr const fault = g_objects.alloc_object(seL4_EndpointObject,
                                                   seL4_EndpointBits,
                                                   greeter_account, &error);
    uint64_t untyped_physical = 0;
    /* 1 MiB: the page tables the greeter's own mapping of its console slice is
     * retyped from, and the pages its freeing heap grows into. The toolkit and
     * its embedded font allocate -- the atlas and the glyph table alone are a
     * few hundred kilobytes -- where the raw greeter it replaced allocated
     * nothing, so 256 KiB ran out and the greeter died before its form. */
    constexpr uint32_t kGreeterUntypedBits = 20;
    seL4_CPtr const untyped = g_objects.carve_untyped(kGreeterUntypedBits,
                                                      greeter_account, &error,
                                                      &untyped_physical);
    if (fault == 0 || untyped == 0) {
        write("      auth: no fault endpoint or untyped for the greeter\n");
        return;
    }
    aegir::spawn::PortGrant const ports[] = {
        {aegir::console::kPortName, aegir::console::kPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared, g_spawn_gui,
         seL4_CapRights_new(1, 1, 0, 1), kGreeterBadge, 0},
        {aegir::auth::kPortName, aegir::auth::kPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared + 1, g_spawn_login,
         seL4_CapRights_new(1, 0, 0, 1), kGreeterBadge, 0},
        {"untyped", 7, aegir::bootstrap::kSlotFirstDeclared + 2, untyped,
         seL4_AllRights, 0, kGreeterUntypedBits},
    };
    static char const kGreeterName[] = "greeter";
    static char const kGreeterBinary[] = "aegir-greeter";
    static char const kGreeterAccount[] = "system";
    aegir::spawn::Request request{};
    request.name = kGreeterName;
    request.name_length = sizeof(kGreeterName) - 1;
    request.binary = kGreeterBinary;
    request.binary_length = sizeof(kGreeterBinary) - 1;
    request.account = kGreeterAccount;
    request.account_length = sizeof(kGreeterAccount) - 1;
    /* A service's priority, not a session's: the greeter shares the serial
     * line with the services, and at a session's priority their every wakeup
     * preempts it -- mid-print, which is how a cue line garbles. At their
     * own priority only a timeslice's end moves it aside, and a line is far
     * shorter than a slice. It blocks on its event channel besides, so the
     * priority costs the boot nothing. */
    request.priority = seL4_MaxPrio - 1;
    request.ports = ports;
    request.port_count = 3;
    request.fault_endpoint = fault;
    request.badge = kGreeterBadge;
    request.give_vspace = true;
    request.untyped_physical = untyped_physical;
    request.untyped_bits = kGreeterUntypedBits;

    aegir::spawn::Initrd const initrd(reinterpret_cast<void const *>(g_binaries_address),
                                      g_binaries_bytes);
    aegir::spawn::Spawner spawner(g_objects, g_scratch, arena, initrd, g_asid_pool,
                                  static_cast<seL4_CPtr>(aegir::bootstrap::kSlotOwnCNode),
                                  aegir::bootstrap::cnode_bits());
    aegir::spawn::Process process{};
    if (!spawner.spawn(request, greeter_account, process)) {
        write("      auth: FAIL spawning the greeter: ");
        write(spawner.problem());
        write("\n");
        return;
    }
    /* Wait for the form: the greeter signals when the cue is printed, and
     * serving -- and the rest of the boot, with its own lines -- starts
     * after, so the runner's cue never shares a serial line with another
     * service's output. */
    g_greeter_supervision = process.supervision;
    seL4_Wait(process.supervision, nullptr);
    g_greeter_up = true;
    write("      auth: the greeter is up\n");
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    aegir::ipc::Consumer const nmspace =
        aegir::ipc::Consumer::find(aegir::nmspace::kPortName,
                                   aegir::nmspace::kPortNameLength);
    aegir::ipc::Owner port = aegir::ipc::Owner::find(aegir::auth::kPortName,
                                                     aegir::auth::kPortNameLength);
    if (!nmspace.valid() || !port.valid()) {
        write("      FAIL auth: no namespace or no port\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    g_nmspace = nmspace;

    /* The spawn kit, adopted the way the partition manager adopts its own
     * (specs/authority.md): the untyped by name -- its size is the grant's,
     * because a service cannot ask the kernel how large an untyped is --
     * the physical base from the block's untyped entry, the VSpace root
     * and the window with it, and the slots past everything the block
     * names. A spawner's memory *is* its delegated untyped: the table and
     * the sessions both come out of it (specs/auth.md). */
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    uint64_t untyped_slot = 0;
    uint32_t untyped_bits = 0;
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            if (entry.kind != aegir::bootstrap::EntryKind::Capability) {
                continue;
            }
            auto const *name = reinterpret_cast<char const *>(block) + entry.data_offset;
            if (entry.length == 7 && name[0] == 'u' && name[1] == 'n' && name[2] == 't' &&
                name[3] == 'y' && name[4] == 'p' && name[5] == 'e' && name[6] == 'd') {
                untyped_slot = entry.number;
                untyped_bits = entry.reserved;
            }
            if (entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            }
        }
    }
    uint64_t untyped_physical = 0;
    uint64_t untyped_address = 0;
    static_cast<void>(aegir::bootstrap::untyped(&untyped_physical, &untyped_bits,
                                                &untyped_address));
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    bool const have_kit =
        untyped_slot != 0 &&
        aegir::bootstrap::capability("vspace", 6, &vspace_slot) &&
        aegir::bootstrap::window(&window_base, &window_bytes);
    if (!have_kit ||
        !g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                 untyped_physical)) {
        write("      FAIL auth: no untyped, vspace or window -- the spawn kit "
              "did not arrive\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    /* The depth is zero because these are *our* slots: at depth zero the
     * destination capability of a retype *is* the CNode
     * (kernel/src/object/untyped.c). The size is the one the spawner builds
     * (kCNodeBits in libs/aegir-spawn/src/process.cc). */
    g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::cnode_bits()) - first_free, 0);
    g_slots_first = first_free;
    g_slots_end = 1u << aegir::bootstrap::cnode_bits();
    if (!g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                         static_cast<uintptr_t>(window_base),
                         static_cast<uintptr_t>(window_base + window_bytes), &g_objects)) {
        write("      FAIL auth: the window would not be adopted\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    aegir::mem::Arena arena(g_objects, g_scratch, g_account);

    /* The slot the database's capability moves into: one of ours, now that
     * the slots past the block are adopted. */
    seL4_CPtr const db_slot = g_objects.alloc_slot();
    if (db_slot == 0) {
        write("      FAIL auth: no slot for the database's capability\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    /* A second slot for the home arc's resolves (specs/auth.md's Homes):
     * deleted after each use, so a login does not spend what the next one
     * needs. */
    g_home_slot = g_objects.alloc_slot();
    /* A slot for the session's mem.main copy (specs/auth.md's Session
     * reclaim): minted fresh for each login's badge, below every session's
     * mark, so it is auth's own and not part of the session's range. */
    g_session_mem_call = g_objects.alloc_slot();
    g_kit_nmspace_slot = g_objects.alloc_slot();
    if (g_home_slot == 0 || g_session_mem_call == 0 || g_kit_nmspace_slot == 0) {
        write("      FAIL auth: no slot for the home or the session's mem.main\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* Resolve Initrd:users.db, asking again until the volume exists -- the
     * volumes join the namespace while the boot set is still coming up. The
     * answer carries the volume-relative rest as a string (an alias may
     * have composed one the path never contained, specs/vfs.md's Aliases),
     * so it lands in a buffer of our own. */
    constexpr char kPath[] = "Initrd:users.db";
    static char rest_buffer[aegir::nmspace::kPathMax];
    seL4_CPtr volume = 0;
    char const *rest = nullptr;
    uint32_t rest_length = 0;
    while (volume == 0) {
        uint64_t out[aegir::nmspace::kPathMax / 8 + 1];
        uint32_t const out_words = aegir::nmspace::pack_string(
            out, kPath, sizeof(kPath) - 1, aegir::nmspace::kPathMax);
        uint64_t in[aegir::nmspace::kResolveWords];
        bool cap_arrived = false;
        aegir::ipc::WordsReply const answer = nmspace.call_transfer(
            aegir::nmspace::kMethodResolve, out, out_words, 0, in,
            aegir::nmspace::kResolveWords, &cap_arrived);
        char const *text = nullptr;
        uint32_t length = 0;
        if (answer.error == 0 && cap_arrived &&
            aegir::nmspace::unpack_string(in, answer.count, aegir::nmspace::kPathMax,
                                          &text, &length) &&
            aegir::ipc::take_received_cap(db_slot)) {
            volume = db_slot;
            for (uint32_t i = 0; i < length; ++i) {
                rest_buffer[i] = text[i];
            }
            rest = rest_buffer;
            rest_length = length;
        } else {
            seL4_Yield();
        }
    }

    /* Read the whole table, one envelope at a time. The first chunk carries
     * the header, which says how many rows follow -- the file is the
     * checksum of its own length -- so the table's room is allocated once,
     * from the arena over the delegated untyped, and each chunk is copied
     * in where it belongs. */
    aegir::ipc::Consumer db(volume);
    uint8_t *table = nullptr;
    uint64_t total = 0;
    uint64_t offset = 0;
    bool read_ok = true;
    for (;;) {
        uint64_t out[aegir::nmspace::kPathMax / 8 + 3];
        uint32_t out_words =
            aegir::nmspace::pack_string(out, rest, rest_length, aegir::nmspace::kPathMax);
        out[out_words++] = offset;
        out[out_words++] = aegir::volume::kReadMax;
        uint64_t in[aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8];
        aegir::ipc::WordsReply const answer = db.call_words(
            aegir::volume::kMethodRead, out, out_words, in,
            aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8);
        if (answer.error != 0 || answer.count < aegir::volume::kReadHeaderWords) {
            read_ok = false;
            break;
        }
        uint64_t const count = in[0];
        uint64_t const eof = in[1];
        if (count > aegir::volume::kReadMax ||
            answer.count < aegir::volume::kReadHeaderWords + (count + 7) / 8) {
            read_ok = false;
            break;
        }
        char const *bytes =
            reinterpret_cast<char const *>(in + aegir::volume::kReadHeaderWords);
        if (offset == 0) {
            /* The first chunk carries the header: magic, version, and the
             * row count that says how long the whole file is. */
            if (count < aegir::authdb::kHeaderBytes) {
                read_ok = false;
                break;
            }
            auto const *header = reinterpret_cast<uint32_t const *>(bytes);
            if (header[0] != aegir::authdb::kMagic ||
                header[1] != aegir::authdb::kVersion) {
                read_ok = false;
                break;
            }
            total = aegir::authdb::kHeaderBytes + header[2] * sizeof(aegir::authdb::Row);
            table = static_cast<uint8_t *>(arena.allocate(total));
            if (table == nullptr) {
                read_ok = false;
                break;
            }
        }
        if (offset + count > total) {
            read_ok = false;
            break;
        }
        for (uint64_t i = 0; i < count; ++i) {
            table[offset + i] = static_cast<uint8_t>(bytes[i]);
        }
        offset += count;
        if (eof != 0 || count == 0) {
            break;
        }
    }

    /* What arrived is the table exactly when it is the length the header
     * promised. */
    if (read_ok && table != nullptr && offset == total) {
        g_rows = reinterpret_cast<aegir::authdb::Row const *>(table +
                                                              aegir::authdb::kHeaderBytes);
        g_users = static_cast<uint32_t>((total - aegir::authdb::kHeaderBytes) /
                                        sizeof(aegir::authdb::Row));
    } else {
        read_ok = false;
    }
    if (!read_ok) {
        write("      FAIL auth: the user database would not read or is not one\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    g_serials = static_cast<uint32_t *>(arena.allocate(sizeof(uint32_t) * g_users));
    if (g_serials == nullptr) {
        write("      FAIL auth: no room for the serial counters\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    for (uint32_t u = 0; u < g_users; ++u) {
        g_serials[u] = 0;
    }

    /* A session no longer has its own pool (specs/auth.md's Session reclaim):
     * its memory comes from the memory service on demand, charged to the
     * session's badge, so there is nothing here to size. */

    /* The rest of the spawn kit: the pool the sessions' address spaces come
     * from, the delegatable copies of what a session needs, and the initrd
     * the session's image is read out of (specs/auth.md). */
    uint64_t pool_slot = 0;
    uint64_t spawn_log_slot = 0;
    uint64_t spawn_nmspace_slot = 0;
    uint64_t spawn_gui_slot = 0;
    uint64_t spawn_login_slot = 0;
    uint64_t gui_slot = 0;
    bool const kit_complete =
        aegir::bootstrap::capability("asid-pool", 9, &pool_slot) &&
        aegir::bootstrap::capability("spawn:log.main", 14, &spawn_log_slot) &&
        aegir::bootstrap::capability("spawn:vfs.namespace", 19, &spawn_nmspace_slot) &&
        aegir::bootstrap::binaries(&g_binaries_address, &g_binaries_bytes) &&
        g_binaries_bytes != 0;
    g_spawn_log = static_cast<seL4_CPtr>(spawn_log_slot);
    g_spawn_nmspace = static_cast<seL4_CPtr>(spawn_nmspace_slot);
    g_asid_pool = static_cast<seL4_CPtr>(pool_slot);
    /* The greeter's kit, separate from the sessions': a boot without it
     * still takes logins over the serial line. */
    if (aegir::bootstrap::capability("spawn:console.gui", 17, &spawn_gui_slot) &&
        aegir::bootstrap::capability("spawn:auth.login", 16, &spawn_login_slot) &&
        aegir::bootstrap::capability("console.gui", 11, &gui_slot)) {
        g_spawn_gui = static_cast<seL4_CPtr>(spawn_gui_slot);
        g_spawn_login = static_cast<seL4_CPtr>(spawn_login_slot);
        g_gui = aegir::ipc::Consumer(static_cast<seL4_CPtr>(gui_slot));
    }
    /* The bureau's own port (specs/workbench.md): present only when the
     * director made the endpoint, which it does because the manifest declares
     * the port. A boot without it still takes logins; the bureau then runs
     * with its own menus and serves none. */
    uint64_t spawn_bureau_menu_slot = 0;
    if (aegir::bootstrap::capability("spawn:bureau.menu", 17, &spawn_bureau_menu_slot)) {
        g_spawn_bureau_menu = static_cast<seL4_CPtr>(spawn_bureau_menu_slot);
    }
    /* The memory service, for the terminals to hand their commands
     * (specs/memory.md Phase 3). Director grants it because the terminal
     * entries need mem.main; a boot without it runs commands on their seed
     * only, which the terminal reports rather than failing. */
    uint64_t spawn_mem_slot = 0;
    if (aegir::bootstrap::capability("spawn:mem.main", 14, &spawn_mem_slot)) {
        g_spawn_mem = static_cast<seL4_CPtr>(spawn_mem_slot);
    }
    /* The clock is optional too: a session without one still runs, and only
     * the tools that ask the time report it (specs/dos.md). Director grants
     * this because the session.terminal entry needs clock.main. */
    uint64_t spawn_clock_slot = 0;
    if (aegir::bootstrap::capability("spawn:clock.main", 16, &spawn_clock_slot)) {
        g_spawn_clock = static_cast<seL4_CPtr>(spawn_clock_slot);
    }
    /* The timer, optional the same way (specs/timer.md). Director grants this
     * because the session.terminal entry needs timer.main. */
    uint64_t spawn_timer_slot = 0;
    if (aegir::bootstrap::capability("spawn:timer.main", 16, &spawn_timer_slot)) {
        g_spawn_timer = static_cast<seL4_CPtr>(spawn_timer_slot);
    }
    aegir::spawn::Initrd const initrd(reinterpret_cast<void const *>(g_binaries_address),
                                      g_binaries_bytes);
    bool const can_spawn = g_spawn_mem != 0 && kit_complete && initrd.valid();
    if (!can_spawn) {
        write("      auth: no pool, delegatable ports, or initrd -- "
              "logins will not start sessions\n");
    }

    write("      auth: ");
    aegir::debug_write_unsigned(g_users);
    write(g_users == 1 ? " user, serving auth.login\n" : " users, serving auth.login\n");
    /* The system's Startup-Sequence, once, before the greeter (specs/boot.md):
     * auth waits for the boot shell's outcome. A failure leaves the read-only
     * view standing instead, and no greeter runs. */
    if (start_boot_session(arena)) {
        start_greeter(arena);
    }
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        seL4_Word caller_badge = 0;
        uint32_t const method = port.receive_words(words, aegir::ipc::kMaxWords, &count,
                                                   &caller_badge);
        if (method == aegir::auth::kMethodLogin) {
            int const user = answer_login(port, words, count);
            if (user >= 0 && can_spawn) {
                bool const from_greeter = g_greeter_up && caller_badge == kGreeterBadge;
                if (from_greeter) {
                    /* A login through the greeter ends the greeter's part. The
                     * exit comes first: the wait is for the second supervision
                     * signal, which the greeter sends as it leaves -- so its
                     * welcome line is written before ours, never across it. Then
                     * the windows and the slice go back before the session
                     * starts -- console's reap, the same teardown order a
                     * session's reclaim follows (specs/console.md). */
                    seL4_Wait(g_greeter_supervision, nullptr);
                    uint64_t const badge_word = kGreeterBadge;
                    uint64_t bin[1];
                    (void)g_gui.call_words(aegir::console::kMethodReap, &badge_word,
                                           1, bin, 1);
                    g_greeter_up = false;
                    write("      auth: the greeter's windows are reaped\n");
                }
                /* The greeter's login starts the bureau; every other caller's
                 * starts the smoke (specs/console.md's login arc). */
                start_session(static_cast<uint32_t>(user), from_greeter);
            }
        } else {
            /* A method this version does not know is answered by saying
             * nothing (specs/services.md's versioning rule). */
            port.reply_words(nullptr, 0);
        }
    }
}
