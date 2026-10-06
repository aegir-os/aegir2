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
#include <aegir/datatypes.h>
#include <aegir/log.h>
#include <aegir/manifest.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/vspace.h>
#include <aegir/memory.h>
#include <aegir/metadata.h>
#include <aegir/nmspace.h>
#include <aegir/process.h>
#include <aegir/process_client.h>
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
/* The network's two ports (specs/net.md), optional like the timer's: a session
 * command may use the stack, so director grants the unbadged sources when the
 * session authority names them. */
seL4_CPtr g_spawn_net_socket = 0;
seL4_CPtr g_spawn_net_control = 0;

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
/* The session's datatypes broker port (specs/datatypes.md): the broker owns it
 * and auth makes the endpoint itself, from the session's own allocator, so a
 * client asks the broker rather than starting a class. Zero when the session
 * declares no broker, and a client starts its class directly. */
seL4_CPtr g_spawn_datatypes = 0;
/* The font service's caller half (specs/fonts.md): the session's windows draw a
 * Sys:Fonts face through it, an OpenType one the toolkit cannot parse itself. */
seL4_CPtr g_spawn_font = 0;
/* The process registry's caller half (specs/process.md): auth registers each
 * service it starts with it, and hands it to a launcher, which registers each
 * of its own children. */
seL4_CPtr g_spawn_process = 0;
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
/* The boot session's launcher (specs/launch.md, specs/boot.md): a distinct
 * system badge so the log and the namespace see it as its own process, while its
 * namespace *copy* is minted for kBootBadge, the principal whose Sys:/C: it and
 * its commands resolve. */
constexpr uint64_t kBootLauncherBadge = 770;

/* The namespace as auth speaks it, and the slot a home resolve's capability
 * lands in -- one slot, deleted after each use, so a login does not spend
 * what the next one needs. */
aegir::ipc::Consumer g_nmspace;
seL4_CPtr g_home_slot = 0;
/* The slot a session-manifest resolve's capability lands in (specs/session.md),
 * its own so it never collides with a home resolve's. */
seL4_CPtr g_manifest_slot = 0;

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

/* Register a just-started child with the process registry (specs/process.md):
 * auth is the spawner, so it records the child's pid, its own badge as the
 * parent, and the name and path the child stands as. Best-effort: a boot whose
 * director delegated no registry leaves auth with none, and it registers
 * nothing. */
void register_child(uint64_t pid, char const *name, uint32_t name_length, char const *path,
                    uint32_t path_length) noexcept
{
    if (g_spawn_process == 0) {
        return;
    }
    uint64_t self = 0;
    if (!aegir::bootstrap::badge(&self)) {
        self = 0;
    }
    aegir::ipc::Consumer const registry(g_spawn_process);
    (void)aegir::process::register_process(registry, pid, self, name, name_length, path,
                                           path_length);
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

/* An alias that is a union of the user's directory and the system's
 * (specs/libraries.md, specs/datatypes.md): the user's member **first**, so it
 * overrides -- a class or library dropped into `Home:DataTypes`/`Home:Libs` is
 * found before the system's, and a name it shares with the system's is the
 * user's. The user's member is the create target, so a write through the alias
 * lands in the user's own tree, where it belongs; the system's is appended and
 * never a create target, because a user does not write the system's directory. */
bool bind_union(uint64_t badge, char const *name, uint32_t name_length,
                char const *home_path, uint32_t home_length, char const *sys_path,
                uint32_t sys_length) noexcept
{
    uint64_t out[2 + aegir::nmspace::kNameMax / 8 + 1 + aegir::nmspace::kPathMax / 8 + 1];
    uint64_t in[1];
    out[0] = badge;
    out[1] = aegir::nmspace::kBindCreate;
    uint32_t words = 2;
    words += aegir::nmspace::pack_string(out + words, name, name_length,
                                         aegir::nmspace::kNameMax);
    words += aegir::nmspace::pack_string(out + words, home_path, home_length,
                                         aegir::nmspace::kPathMax);
    aegir::ipc::WordsReply const first =
        g_nmspace.call_words(aegir::nmspace::kMethodBind, out, words, in, 1);
    if (first.error != 0 || first.count != 1 || in[0] != 1) {
        return false;
    }
    out[1] = aegir::nmspace::kBindAppend;
    words = 2;
    words += aegir::nmspace::pack_string(out + words, name, name_length,
                                         aegir::nmspace::kNameMax);
    words += aegir::nmspace::pack_string(out + words, sys_path, sys_length,
                                         aegir::nmspace::kPathMax);
    aegir::ipc::WordsReply const second =
        g_nmspace.call_words(aegir::nmspace::kMethodBind, out, words, in, 1);
    return second.error == 0 && second.count == 1 && in[0] == 1;
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
            /* The home's class and resource-library directories
             * (specs/datatypes.md, specs/libraries.md): the user's members of
             * the `DataTypes:` and `LIBS:` unions, where a class or library the
             * user drops in is found before the system's. The same ownership,
             * so only the user may write them. */
            if (made &&
                !make_owned_home_subdir(volume, rest, rest_length, "/DataTypes",
                                        sizeof("/DataTypes") - 1, user)) {
                write("      auth: FAIL the class directory would not be made\n");
            }
            if (made &&
                !make_owned_home_subdir(volume, rest, rest_length, "/Libs",
                                        sizeof("/Libs") - 1, user)) {
                write("      auth: FAIL the library directory would not be made\n");
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

    /* DataTypes: and LIBS: (specs/datatypes.md, specs/libraries.md): the class
     * and resource-library unions. The user's `Home:DataTypes`/`Home:Libs` is
     * first, so a class or library dropped in is found before the system's and
     * a shared name is the user's -- the add-on property, and an override, the
     * same shape `ENV:` has. The directories are made and owned above. */
    if (!bind_union(badge, "DataTypes", 9, "Home:DataTypes", 14, "Sys:DataTypes", 13)) {
        write("      auth: FAIL the DataTypes: union was refused\n");
    }
    if (!bind_union(badge, "LIBS", 4, "Home:Libs", 9, "Sys:Libs", 8)) {
        write("      auth: FAIL the LIBS: union was refused\n");
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
void reclaim_session(uint64_t badge, uint32_t direct_badges, seL4_CPtr mark,
                     uintptr_t scratch_mark,
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
     * session minted directly revokes every chunk that badge owns -- the
     * session's objects, its spawn's staging, and each service's and the
     * launcher's runtime untypeds, all retyped from those chunks -- and with
     * them go the minted port copies in their CSpaces (specs/authority.md's
     * retained-copy path, specs/memory.md). Every directly-minted badge is
     * released, not only the session's and the terminal's: a three-service
     * session mints the bureau, the terminal, the datatypes broker and the
     * launcher, and each owns its runtime. The kernel unmaps a mapped frame
     * when the cap goes (finaliseCap), so the staging's scratch-window pages
     * are already unmapped; the window's cursor just needs to be told. The
     * slots past the mark are empty, so the cursor returns to it. */
    aegir::ipc::Consumer const mem(g_spawn_mem);
    uint64_t released = 0;
    for (uint32_t i = 0; i < direct_badges; ++i) {
        uint64_t const owner = badge + i;
        (void)mem.call_words(aegir::memory::kMethodRelease, &owner, 1, &released, 1);
    }
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

/* A C string's length. */
uint32_t text_length(char const *text) noexcept
{
    uint32_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

/* A launcher's identity and context (specs/launch.md): where a session's and the
 * boot session's launcher differ. The strings are static, because the spawn
 * copies them synchronously. */
struct LauncherSpec {
    uint64_t badge = 0;
    uint64_t namespace_badge = 0;
    char const *name = nullptr;
    char const *account = nullptr;
    uint32_t account_length = 0;
    char const *cwd = nullptr;
    char const *const *environment = nullptr;
    uint32_t environment_count = 0;
};

/* Spawn a launcher (specs/launch.md): the process that holds a spawn kit and
 * serves `launch.session`, whose owner half auth made and passes as
 * `launch_port`. `mem` is where its fault endpoint and runtime untyped come
 * from -- a session's pool or auth's own -- and `account` what they are charged
 * to; `base_kit` the delegates it hands its children. Its namespace copy is
 * minted here for `spec.namespace_badge`, so the boot session's commands resolve
 * Sys:/C: and a user's resolve Home:/ENV:. It gets the caller half of its own
 * endpoint (`spawn:launch.session`), so it can hand a nested terminal the same
 * half a shell is given, and its own badged mem.main, so staging megabytes does
 * not exhaust its fixed seed. True when it spawned; a failure is logged. */
bool spawn_launcher(aegir::mem::Allocator &mem, aegir::mem::Account &account,
                    aegir::spawn::Spawner &spawner, aegir::spawn::Kit const &base_kit,
                    LauncherSpec const &spec, seL4_CPtr launch_port,
                    seL4_CPtr *supervision = nullptr) noexcept
{
    seL4_Error fault_error = seL4_NoError;
    seL4_CPtr const fault = mem.alloc_object(seL4_EndpointObject, seL4_EndpointBits,
                                             account, &fault_error);
    constexpr uint32_t kLauncherUntypedBits = 22;
    seL4_Error untyped_error = seL4_NoError;
    uint64_t untyped_physical = 0;
    seL4_CPtr const untyped = mem.carve_untyped(kLauncherUntypedBits, account,
                                                &untyped_error, &untyped_physical);
    if (fault == 0 || untyped == 0) {
        write("      auth: FAIL no memory for the launcher\n");
        return false;
    }
    if (!mint_session_nmspace(g_kit_nmspace_slot, spec.namespace_badge)) {
        write("      auth: FAIL no namespace for the launcher\n");
        return false;
    }
    aegir::spawn::Kit kit = base_kit;
    kit.nmspace = g_kit_nmspace_slot;
    /* The launcher registers every child it starts (specs/process.md), so it is
     * handed the process.registry caller half, which launcher_ports grants. */
    kit.process_registry = g_spawn_process;
    aegir::spawn::Child child{};
    child.badge = spec.badge;
    child.runtime = untyped;
    child.runtime_bits = kLauncherUntypedBits;
    child.launcher = true;
    /* launcher_ports fills up to 16 (four identity grants plus twelve), and a
     * launcher's own ports are appended below; size past both. */
    aegir::spawn::PortGrant ports[24];
    uint32_t count = aegir::spawn::launcher_ports(kit, child, ports, 24);
    /* The endpoint's owner half: the launcher serves on it. An owner needs Read
     * to receive; the caller's halves carry Write instead, the other side of the
     * same endpoint. */
    ports[count] = {aegir::launch::kPortName, aegir::launch::kPortNameLength,
                    aegir::bootstrap::kSlotFirstDeclared + count, launch_port,
                    seL4_CapRights_new(0, 0, 1, 0), 0, 0};
    ++count;
    /* Its own caller half, so a nested terminal is handed the same half a shell
     * is given and its commands go through the one launcher too. */
    ports[count] = {"spawn:launch.session", 20,
                    aegir::bootstrap::kSlotFirstDeclared + count, launch_port,
                    seL4_CapRights_new(1, 1, 0, 1), spec.badge, 0};
    ++count;
    /* Its own badged mem.main: its heap grows through it, as a command's does. */
    ports[count] = {aegir::memory::kPortName, aegir::memory::kPortNameLength,
                    aegir::bootstrap::kSlotFirstDeclared + count, kit.mem_main,
                    seL4_CapRights_new(1, 1, 0, 1), spec.badge, 0};
    ++count;
    /* The bureau.menu *source* (specs/workbench.md): the launcher mints each of
     * its commands a caller half from it, so a launched program can register
     * the menus the screen bar shows while it is active -- the Workbench model,
     * where until now only the demo, a boot service, could. Unbadged, because a
     * badged cap cannot be minted again; a command's own badge is what the
     * bureau reads. A boot whose director made no such port passes none. */
    if (g_spawn_bureau_menu != 0) {
        ports[count] = {"bureau.menu", 11,
                        aegir::bootstrap::kSlotFirstDeclared + count, g_spawn_bureau_menu,
                        seL4_AllRights, 0, 0};
        ++count;
    }
    /* The unbadged font.main source (specs/fonts.md): the launcher mints each of
     * its commands a caller half from it, the same shape as the menu above. */
    if (g_spawn_font != 0) {
        ports[count] = {"font.main", 9,
                        aegir::bootstrap::kSlotFirstDeclared + count, g_spawn_font,
                        seL4_AllRights, 0, 0};
        ++count;
    }
    /* The unbadged datatypes.main source (specs/datatypes.md): the launcher
     * mints each of its commands a caller half from it, so a launched program
     * asks the session's broker to open a file. Absent when the session declares
     * no broker, and a client starts its class directly. */
    if (g_spawn_datatypes != 0) {
        ports[count] = {aegir::datatypes::kBrokerPortName,
                        aegir::datatypes::kBrokerPortNameLength,
                        aegir::bootstrap::kSlotFirstDeclared + count, g_spawn_datatypes,
                        seL4_AllRights, 0, 0};
        ++count;
    }
    aegir::spawn::Request request{};
    request.name = spec.name;
    request.name_length = text_length(spec.name);
    static char const kLauncherBinary[] = "aegir-launcher";
    request.binary = kLauncherBinary;
    request.binary_length = sizeof(kLauncherBinary) - 1;
    request.account = spec.account;
    request.account_length = spec.account_length;
    request.cwd = spec.cwd;
    request.cwd_length = text_length(spec.cwd);
    request.environment = spec.environment;
    request.environment_count = spec.environment_count;
    request.priority = seL4_MaxPrio - 2;
    request.ports = ports;
    request.port_count = count;
    request.fault_endpoint = fault;
    request.badge = spec.badge;
    request.give_vspace = true;
    /* A launcher stages images of its own (a nested terminal's is near a
     * megabyte) and holds every live command's caps at once -- the shell, the
     * demo, a nested terminal and the datatype classes a session keeps -- so
     * its CSpace is two-level (specs/memory.md): a root CNode of 8 bits over L2
     * CNodes of 12, its own caps in the L2 CNode at root slot 0 and each
     * command given its own L2 CNode, so the capacity grows with live commands
     * instead of running out at one CNode. */
    request.cnode_bits = 12;
    request.cspace_l1_bits = 8;
    request.untyped_physical = untyped_physical;
    request.untyped_bits = kLauncherUntypedBits;
    aegir::spawn::Process process{};
    if (!spawner.spawn(request, account, process)) {
        write("      auth: FAIL spawning the launcher: ");
        write(spawner.problem());
        char const *const detail = spawner.detail();
        if (detail != nullptr && detail[0] != '\0') {
            write(" (");
            write(detail);
            write(")");
        }
        write("\n");
        return false;
    }
    /* The launcher is in the live set (specs/process.md): auth is its spawner. */
    register_child(spec.badge, spec.name, text_length(spec.name), nullptr, 0);
    if (supervision != nullptr) {
        *supervision = process.supervision;
    }
    return true;
}

/* Read a whole file through the namespace into `buffer`, up to `capacity`
 * bytes: resolve the path to a volume, then read it one envelope at a time --
 * the shape the user database is read with (specs/vfs.md). Answer the length,
 * or 0 when the file is absent, unreadable, or larger than the buffer. */
uint32_t read_file(char const *path, uint32_t path_length, char *buffer,
                   uint32_t capacity) noexcept
{
    if (g_manifest_slot == 0) {
        return 0;
    }
    uint64_t out[aegir::nmspace::kPathMax / 8 + 1];
    uint32_t const out_words =
        aegir::nmspace::pack_string(out, path, path_length, aegir::nmspace::kPathMax);
    uint64_t in[aegir::nmspace::kResolveWords];
    bool cap_arrived = false;
    aegir::ipc::WordsReply const resolved = g_nmspace.call_transfer(
        aegir::nmspace::kMethodResolve, out, out_words, 0, in,
        aegir::nmspace::kResolveWords, &cap_arrived);
    char rest[aegir::nmspace::kPathMax];
    uint32_t rest_length = 0;
    char const *text = nullptr;
    uint32_t length = 0;
    if (out_words == 0 || resolved.error != 0 || !cap_arrived ||
        !aegir::nmspace::unpack_string(in, resolved.count, aegir::nmspace::kPathMax,
                                       &text, &length) ||
        !aegir::ipc::take_received_cap(g_manifest_slot)) {
        return 0;
    }
    for (uint32_t i = 0; i < length; ++i) {
        rest[i] = text[i];
    }
    rest_length = length;

    aegir::ipc::Consumer const file(g_manifest_slot);
    uint32_t total = 0;
    for (;;) {
        uint64_t read_out[aegir::nmspace::kPathMax / 8 + 2];
        uint32_t read_words = aegir::nmspace::pack_string(
            read_out, rest, rest_length, aegir::nmspace::kPathMax);
        if (read_words == 0) {
            total = 0;
            break;
        }
        read_out[read_words++] = total;
        read_out[read_words++] = aegir::volume::kReadMax;
        uint64_t read_in[aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8];
        aegir::ipc::WordsReply const answer = file.call_words(
            aegir::volume::kMethodRead, read_out, read_words, read_in,
            aegir::volume::kReadHeaderWords + aegir::volume::kReadMax / 8);
        if (answer.error != 0 || answer.count < aegir::volume::kReadHeaderWords) {
            total = 0;
            break;
        }
        uint64_t const count = read_in[0];
        uint64_t const eof = read_in[1];
        if (count > aegir::volume::kReadMax ||
            answer.count < aegir::volume::kReadHeaderWords + (count + 7) / 8 ||
            total + count > capacity) {
            total = 0;
            break;
        }
        char const *bytes =
            reinterpret_cast<char const *>(read_in + aegir::volume::kReadHeaderWords);
        for (uint64_t i = 0; i < count; ++i) {
            buffer[total + i] = bytes[i];
        }
        total += static_cast<uint32_t>(count);
        if (eof != 0 || count == 0) {
            break;
        }
    }
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_manifest_slot,
                      aegir::bootstrap::cnode_bits());
    return total;
}

/* A file's size through the namespace, or false when it is not there. It sizes
 * the buffer a manifest goes into, so nothing guesses at a ceiling. */
bool file_size(char const *path, uint32_t path_length, uint64_t *size) noexcept
{
    if (g_manifest_slot == 0) {
        return false;
    }
    uint64_t out[aegir::nmspace::kPathMax / 8 + 1];
    uint32_t const out_words =
        aegir::nmspace::pack_string(out, path, path_length, aegir::nmspace::kPathMax);
    uint64_t in[aegir::nmspace::kResolveWords];
    bool cap_arrived = false;
    aegir::ipc::WordsReply const resolved = g_nmspace.call_transfer(
        aegir::nmspace::kMethodResolve, out, out_words, 0, in,
        aegir::nmspace::kResolveWords, &cap_arrived);
    char rest[aegir::nmspace::kPathMax];
    uint32_t rest_length = 0;
    char const *text = nullptr;
    uint32_t length = 0;
    if (out_words == 0 || resolved.error != 0 || !cap_arrived ||
        !aegir::nmspace::unpack_string(in, resolved.count, aegir::nmspace::kPathMax,
                                       &text, &length) ||
        !aegir::ipc::take_received_cap(g_manifest_slot)) {
        return false;
    }
    for (uint32_t i = 0; i < length; ++i) {
        rest[i] = text[i];
    }
    rest_length = length;
    aegir::ipc::Consumer const file(g_manifest_slot);
    uint64_t stat_out[aegir::nmspace::kPathMax / 8 + 1];
    uint32_t const stat_words =
        aegir::nmspace::pack_string(stat_out, rest, rest_length, aegir::nmspace::kPathMax);
    uint64_t stat_in[aegir::volume::kStatTailWords];
    aegir::ipc::WordsReply const answer = file.call_words(
        aegir::volume::kMethodStat, stat_out, stat_words, stat_in,
        aegir::volume::kStatTailWords);
    bool const ok = stat_words != 0 && answer.error == 0 &&
                    answer.count >= aegir::volume::kStatTailWords &&
                    stat_in[0] == aegir::volume::kKindFile;
    if (ok) {
        *size = stat_in[1];
    }
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_manifest_slot,
                      aegir::bootstrap::cnode_bits());
    return ok;
}

/* The session's manifest (specs/session.md): the user's `Home:S/session.manifest`
 * if it exists, else the shipped `Sys:S/session.manifest`. Read it into memory
 * the arena owns -- the text must outlive the parse, because the entries' views
 * point into it -- and answer it with its length, or nullptr when neither is
 * there. The buffer is sized by the file's own stat, so no ceiling is guessed. */
char *read_session_manifest(aegir::mem::Arena &arena, uint32_t *length) noexcept
{
    static char const kUser[] = "Home:S/session.manifest";
    static char const kSystem[] = "Sys:S/session.manifest";
    char const *const paths[2] = {kUser, kSystem};
    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t const path_length = text_length(paths[i]);
        uint64_t size = 0;
        if (!file_size(paths[i], path_length, &size) || size == 0) {
            continue;
        }
        char *const buffer = static_cast<char *>(arena.allocate(size + 1));
        if (buffer == nullptr) {
            return nullptr;
        }
        uint32_t const n =
            read_file(paths[i], path_length, buffer, static_cast<uint32_t>(size + 1));
        if (n == 0) {
            continue;
        }
        buffer[n] = '\0';
        *length = n;
        return buffer;
    }
    *length = 0;
    return nullptr;
}

/* A session service resolved from its manifest entry (specs/session.md), or the
 * built-in default when the manifest is absent or malformed. `name`/`binary` are
 * NUL-terminated copies; `needs`/`owns` point into the manifest text, which lives
 * for the session's start. `launcher` is the shape: a service that delegates
 * memory to its own children gets a spawn kit; anything else is windowed, and its
 * ports are what `needs` names and the ports it `owns`. */
struct SessionService {
    char name[32];
    char binary[32];
    uint32_t binary_length;
    bool launcher;
    bool maps;
    uint32_t memory_bits;
    uint32_t cspace_bits;
    uint32_t delegate_bits;
    aegir::manifest::View needs;
    aegir::manifest::View owns;
};

void copy_text(char *out, uint32_t capacity, char const *text, uint32_t length) noexcept
{
    uint32_t const n = length < capacity - 1 ? length : capacity - 1;
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = text[i];
    }
    out[n] = '\0';
}

/* KiB and MiB to the bit count a carved untyped is named by. The parser rounds
 * each to a power of two, so the log2 is exact; zero stays zero. */
uint32_t kib_bits(uint32_t kib) noexcept
{
    if (kib == 0) {
        return 0;
    }
    uint32_t bits = 10;
    while (kib > 1) {
        kib >>= 1;
        ++bits;
    }
    return bits;
}

uint32_t mib_bits(uint32_t mib) noexcept
{
    if (mib == 0) {
        return 0;
    }
    uint32_t bits = 20;
    while (mib > 1) {
        mib >>= 1;
        ++bits;
    }
    return bits;
}

/* The built-in minimum (specs/session.md): a terminal, when the manifest is
 * absent or malformed. It is launcher-shaped -- it owns the console stream, so
 * its commands go through the session launcher -- with the defaults the shipped
 * terminal carries. */
void default_terminal(SessionService *out) noexcept
{
    static char const kName[] = "terminal";
    static char const kBinary[] = "aegir-terminal";
    copy_text(out->name, sizeof(out->name), kName, sizeof(kName) - 1);
    copy_text(out->binary, sizeof(out->binary), kBinary, sizeof(kBinary) - 1);
    out->binary_length = sizeof(kBinary) - 1;
    out->launcher = true;
    out->maps = true;
    out->memory_bits = 22;
    out->cspace_bits = 13;
    out->delegate_bits = 22;
    out->needs = {nullptr, 0};
    out->owns = {nullptr, 0};
}

bool describe_service(aegir::manifest::Entry const &entry, SessionService *out) noexcept
{
    if (entry.authority != aegir::manifest::Authority::User) {
        return false;
    }
    if (entry.binary.length == 0) {
        return false;
    }
    copy_text(out->name, sizeof(out->name), entry.name.data, entry.name.length);
    copy_text(out->binary, sizeof(out->binary), entry.binary.data, entry.binary.length);
    out->binary_length = entry.binary.length;
    out->launcher = entry.delegate_mib > 0;
    out->maps = entry.maps;
    out->memory_bits = kib_bits(entry.memory_kib);
    out->cspace_bits = entry.cspace_bits;
    out->delegate_bits = mib_bits(entry.delegate_mib);
    out->needs = entry.needs;
    out->owns = entry.owns;
    return true;
}

/* The capability one `needs` name is met with (specs/session.md): the cap auth
 * already holds, the rights it is installed with, and the badge it carries.
 * False when the name is outside the vocabulary auth can satisfy today. */
bool need_grant(aegir::manifest::View need, seL4_CPtr launch_port, uint64_t badge,
                uint64_t namespace_badge, seL4_CPtr *cap, seL4_CapRights_t *rights,
                uint64_t *cap_badge) noexcept
{
    if (aegir::manifest::equals(need, "log.main")) {
        *cap = g_spawn_log;
        *rights = seL4_CapRights_new(1, 0, 0, 1);
        *cap_badge = badge;
        return g_spawn_log != 0;
    }
    if (aegir::manifest::equals(need, "vfs.namespace")) {
        /* The session's namespace, not the service's own (specs/session.md): a
         * session binds one namespace for its badge and every program resolves
         * through it, so a service's file reads are the session's and one alias
         * set serves them all. The launcher already does this; the services do
         * too, now. */
        *cap = g_spawn_nmspace;
        *rights = seL4_CapRights_new(1, 1, 0, 1);
        *cap_badge = namespace_badge;
        return g_spawn_nmspace != 0;
    }
    if (aegir::manifest::equals(need, "console.gui")) {
        *cap = g_spawn_gui;
        *rights = seL4_CapRights_new(1, 1, 0, 1);
        *cap_badge = badge;
        return g_spawn_gui != 0;
    }
    if (aegir::manifest::equals(need, "font.main")) {
        *cap = g_spawn_font;
        *rights = seL4_CapRights_new(1, 1, 0, 1);
        *cap_badge = badge;
        return g_spawn_font != 0;
    }
    if (aegir::manifest::equals(need, "process.registry")) {
        *cap = g_spawn_process;
        *rights = seL4_CapRights_new(1, 1, 0, 1);
        *cap_badge = badge;
        return g_spawn_process != 0;
    }
    if (aegir::manifest::equals(need, "clock.main")) {
        *cap = g_spawn_clock;
        *rights = seL4_CapRights_new(1, 0, 0, 1);
        *cap_badge = 0;
        return g_spawn_clock != 0;
    }
    if (aegir::manifest::equals(need, "timer.main")) {
        *cap = g_spawn_timer;
        *rights = seL4_CapRights_new(1, 0, 0, 1);
        *cap_badge = 0;
        return g_spawn_timer != 0;
    }
    /* The network (specs/net.md): a session service may name the socket and
     * control ports, so a session command reaches the stack. */
    if (aegir::manifest::equals(need, "net.socket")) {
        *cap = g_spawn_net_socket;
        *rights = seL4_CapRights_new(1, 0, 0, 1);
        *cap_badge = badge;
        return g_spawn_net_socket != 0;
    }
    if (aegir::manifest::equals(need, "net.control")) {
        *cap = g_spawn_net_control;
        *rights = seL4_CapRights_new(1, 0, 0, 1);
        *cap_badge = badge;
        return g_spawn_net_control != 0;
    }
    if (aegir::manifest::equals(need, "mem.main")) {
        *cap = g_spawn_mem;
        *rights = seL4_CapRights_new(1, 1, 0, 1);
        *cap_badge = badge;
        return g_spawn_mem != 0;
    }
    if (aegir::manifest::equals(need, "launch.session")) {
        *cap = launch_port;
        *rights = seL4_CapRights_new(1, 1, 0, 1);
        *cap_badge = badge;
        return launch_port != 0;
    }
    if (aegir::manifest::equals(need, "bureau.menu")) {
        *cap = g_spawn_bureau_menu;
        *rights = seL4_CapRights_new(1, 1, 0, 1);
        *cap_badge = badge;
        return g_spawn_bureau_menu != 0;
    }
    return false;
}

/* Append one grant per `needs` name. Answer false, having written the reason, on
 * a name outside the vocabulary or one auth holds no cap for. */
bool append_needs(aegir::manifest::View needs, seL4_CPtr launch_port, uint64_t badge,
                  uint64_t namespace_badge, aegir::spawn::PortGrant *ports,
                  uint32_t capacity, uint32_t *count) noexcept
{
    uint32_t at = 0;
    while (at < needs.length) {
        while (at < needs.length && (needs.data[at] == ' ' || needs.data[at] == '\t')) {
            ++at;
        }
        uint32_t const start = at;
        while (at < needs.length && needs.data[at] != ',') {
            ++at;
        }
        uint32_t end = at;
        while (end > start && (needs.data[end - 1] == ' ' || needs.data[end - 1] == '\t')) {
            --end;
        }
        if (end > start) {
            aegir::manifest::View const name{needs.data + start, end - start};
            seL4_CPtr cap = 0;
            seL4_CapRights_t rights = seL4_CapRights_new(0, 0, 0, 0);
            uint64_t cap_badge = 0;
            if (!need_grant(name, launch_port, badge, namespace_badge, &cap, &rights,
                            &cap_badge)) {
                write("      auth: session.manifest: auth cannot satisfy need '");
                write(name.data, name.length);
                write("'\n");
                return false;
            }
            if (*count >= capacity) {
                write("      auth: FAIL too many ports on a session service\n");
                return false;
            }
            ports[*count] = {name.data, name.length,
                             aegir::bootstrap::kSlotFirstDeclared + *count, cap, rights,
                             cap_badge, 0};
            ++*count;
        }
        if (at < needs.length && needs.data[at] == ',') {
            ++at;
        }
    }
    return true;
}

/* The capability auth made for an `owns` name, making the endpoint now if it is
 * not yet (specs/session.md): the owner half a session service serves, or the
 * source the launcher mints a caller from. `known` says whether the name is one
 * auth can compose at all; only the session's own ports are. */
seL4_CPtr own_endpoint(aegir::manifest::View name, aegir::mem::Account &account,
                       bool *known) noexcept
{
    *known = true;
    seL4_CPtr *slot = nullptr;
    if (aegir::manifest::equals(name, "bureau.menu")) {
        slot = &g_spawn_bureau_menu;
    } else if (aegir::manifest::equals(name, "datatypes.main")) {
        slot = &g_spawn_datatypes;
    } else {
        *known = false;
        return 0;
    }
    if (*slot == 0) {
        seL4_Error error = seL4_NoError;
        *slot = g_session_mem.alloc_object(seL4_EndpointObject, seL4_EndpointBits,
                                           account, &error);
    }
    return *slot;
}

/* Append the owner half of each port the service `owns` (specs/session.md). auth
 * makes the endpoint (own_endpoint); the owner reads it, unbadged. A name auth
 * cannot compose is the manifest's error, not a guess. */
bool append_owns(aegir::manifest::View owns, aegir::mem::Account &account,
                 aegir::spawn::PortGrant *ports, uint32_t capacity,
                 uint32_t *count) noexcept
{
    uint32_t at = 0;
    while (at < owns.length) {
        while (at < owns.length && (owns.data[at] == ' ' || owns.data[at] == '\t')) {
            ++at;
        }
        uint32_t const start = at;
        while (at < owns.length && owns.data[at] != ',') {
            ++at;
        }
        uint32_t end = at;
        while (end > start && (owns.data[end - 1] == ' ' || owns.data[end - 1] == '\t')) {
            --end;
        }
        if (end > start) {
            aegir::manifest::View const name{owns.data + start, end - start};
            bool known = false;
            seL4_CPtr const cap = own_endpoint(name, account, &known);
            if (!known || cap == 0) {
                write("      auth: session.manifest: auth cannot make own '");
                write(name.data, name.length);
                write("'\n");
                return false;
            }
            if (*count >= capacity) {
                write("      auth: FAIL too many ports on a session service\n");
                return false;
            }
            ports[*count] = {name.data, name.length,
                             aegir::bootstrap::kSlotFirstDeclared + *count, cap,
                             seL4_CanRead, 0, 0};
            ++*count;
        }
        if (at < owns.length && owns.data[at] == ',') {
            ++at;
        }
    }
    return true;
}

/* Resolve the session's services from its manifest text (specs/session.md).
 * Answer an array the arena owns, with `count_out` set: zero services is the
 * launcher's session alone. A malformed text, or a service auth cannot compose,
 * answers the built-in terminal with one service, so the caller recovers. */
SessionService *resolve_services(aegir::mem::Arena &arena, aegir::mem::Account &account,
                                 char const *text, uint32_t length,
                                 uint32_t *count_out) noexcept
{
    aegir::manifest::Manifest parsed(arena, account);
    if (!parsed.parse(text, length)) {
        aegir::manifest::Manifest::Problem const problem = parsed.problem();
        char line_text[20];
        uint32_t const line_length = decimal(line_text, problem.line);
        write("      auth: session.manifest: line ");
        write(line_text, line_length);
        write(": ");
        write(problem.message);
        write("\n");
        SessionService *fallback =
            static_cast<SessionService *>(arena.allocate(sizeof(SessionService)));
        if (fallback != nullptr) {
            default_terminal(&fallback[0]);
            *count_out = 1;
        }
        return fallback;
    }
    uint32_t const capacity = parsed.size() == 0 ? 1 : parsed.size();
    SessionService *services =
        static_cast<SessionService *>(arena.allocate(sizeof(SessionService) * capacity));
    if (services == nullptr) {
        return nullptr;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < parsed.size(); ++i) {
        if (!describe_service(parsed[i], &services[count])) {
            write("      auth: session.manifest: [");
            write(parsed[i].name.data, parsed[i].name.length);
            write("]: authority must be user, and binary is required\n");
            default_terminal(&services[0]);
            *count_out = 1;
            return services;
        }
        ++count;
    }
    char count_text[20];
    uint32_t const count_length = decimal(count_text, count);
    write("      auth: session.manifest: ");
    write(count_text, count_length);
    write(" service(s)\n");
    *count_out = count;
    return services;
}

/* Start one session service (specs/session.md): a launcher-shaped one gets the
 * spawn kit; a windowed one gets its `needs` and its `owns`. It is spawned as
 * the session's user class, over the session pool, under `session.<name>`. The
 * first long-lived child's supervision comes back so the caller can wait on it.
 * False, having written the reason, when it will not start. */
bool spawn_service(SessionService const &spec, uint32_t user, uint32_t range_base,
                   uint64_t badge, uint64_t namespace_badge, seL4_CPtr launch_port,
                   aegir::spawn::Spawner &spawner, aegir::mem::Account &account,
                   seL4_CPtr *supervision) noexcept
{
    seL4_Error fault_error = seL4_NoError;
    seL4_CPtr const fault = g_session_mem.alloc_object(
        seL4_EndpointObject, seL4_EndpointBits, account, &fault_error);
    if (fault == 0) {
        write("      auth: FAIL no fault endpoint for session.");
        write(spec.name);
        write("\n");
        return false;
    }
    seL4_Error untyped_error = seL4_NoError;
    uint64_t untyped_physical = 0;
    seL4_CPtr untyped = 0;
    if (spec.maps) {
        untyped = g_session_mem.carve_untyped(spec.memory_bits, account, &untyped_error,
                                              &untyped_physical);
        if (untyped == 0) {
            write("      auth: FAIL no untyped for session.");
            write(spec.name);
            write("\n");
            return false;
        }
    }
    aegir::spawn::PortGrant ports[24];
    uint32_t count = 0;
    if (spec.launcher) {
        if (!mint_session_nmspace(g_kit_nmspace_slot, namespace_badge)) {
            write("      auth: FAIL no namespace copy for session.");
            write(spec.name);
            write("\n");
            return false;
        }
        aegir::spawn::Kit kit{};
        kit.log = g_spawn_log;
        kit.console_gui = g_spawn_gui;
        kit.mem_main = g_spawn_mem;
        kit.asid_pool = g_asid_pool;
        kit.clock = g_spawn_clock;
        kit.timer = g_spawn_timer;
        kit.net_socket = g_spawn_net_socket;
        kit.net_control = g_spawn_net_control;
        kit.nmspace = g_kit_nmspace_slot;
        kit.font_main = g_spawn_font;
        /* A launcher-shaped service (the session's terminal) starts the shell
         * and its commands, so it registers them (specs/process.md). */
        kit.process_registry = g_spawn_process;
        aegir::spawn::Child child{};
        child.badge = badge;
        child.runtime = untyped;
        child.runtime_bits = spec.memory_bits;
        if (spec.delegate_bits != 0) {
            seL4_Error pool_error = seL4_NoError;
            uint64_t pool_physical = 0;
            child.shell_pool = g_session_mem.carve_untyped(spec.delegate_bits, account,
                                                           &pool_error, &pool_physical);
            child.shell_pool_bits = spec.delegate_bits;
            if (child.shell_pool == 0) {
                write("      auth: FAIL no shell pool for session.");
                write(spec.name);
                write("\n");
                return false;
            }
        }
        child.launcher = true;
        count = aegir::spawn::launcher_ports(kit, child, ports, 16);
        ports[count] = {aegir::launch::kPortName, aegir::launch::kPortNameLength,
                        aegir::bootstrap::kSlotFirstDeclared + count, launch_port,
                        seL4_CapRights_new(1, 1, 0, 1), badge, 0};
        ++count;
    } else {
        if (!append_needs(spec.needs, launch_port, badge, namespace_badge, ports, 24,
                          &count)) {
            return false;
        }
        if (spec.maps) {
            ports[count] = {"untyped", 7, aegir::bootstrap::kSlotFirstDeclared + count,
                            untyped, seL4_AllRights, 0, spec.memory_bits};
            ++count;
        }
        if (!append_owns(spec.owns, account, ports, 24, &count)) {
            return false;
        }
    }

    static char const kPrefix[] = "session.";
    char name_buffer[40];
    uint32_t name_at = 0;
    for (uint32_t i = 0; i < sizeof(kPrefix) - 1; ++i) {
        name_buffer[name_at++] = kPrefix[i];
    }
    uint32_t const name_length = text_length(spec.name);
    for (uint32_t i = 0; i < name_length && name_at < sizeof(name_buffer) - 1; ++i) {
        name_buffer[name_at++] = spec.name[i];
    }
    name_buffer[name_at] = '\0';

    static char const kHomeCwd[] = "Home:";
    aegir::spawn::Request request{};
    request.name = name_buffer;
    request.name_length = name_at;
    request.binary = spec.binary;
    request.binary_length = spec.binary_length;
    request.account = g_rows[user].account;
    request.account_length = field_length(g_rows[user].account, aegir::authdb::kAccountBytes);
    request.cwd = kHomeCwd;
    request.cwd_length = sizeof(kHomeCwd) - 1;
    request.priority = seL4_MaxPrio - 2;
    request.ports = ports;
    request.port_count = count;
    request.fault_endpoint = fault;
    request.badge = badge;
    request.cnode_bits = spec.cspace_bits;
    request.give_vspace = spec.maps;
    request.untyped_physical = untyped_physical;
    request.untyped_bits = spec.memory_bits;
    /* A launcher-shaped service is the session's shell host, and is marked as
     * the session's own composition by an argument, so its shell runs
     * `Home:S/User-Startup` once (specs/session.md). An argument, not an
     * environment entry: a child inherits its parent's environment, so a nested
     * shell would inherit an env mark and run User-Startup again -- the
     * per-shell bug the once rule exists to avoid. The launcher auth starts
     * itself carries no mark, and a nested terminal is started by the launcher,
     * so neither runs it. The same service also hands its children a serial
     * range (specs/launch.md): the serials past the session's own children, so
     * no two of a session's processes share one. */
    char const *environment[1] = {nullptr};
    if (spec.launcher) {
        static char const kSessionMark[] = "--session";
        static char const *const kSessionArguments[1] = {kSessionMark};
        request.arguments = kSessionArguments;
        request.argument_count = 1;
        environment[0] = badge_range_env(range_base, aegir::ipc::kSessionSerialStride - 3);
        request.environment = environment;
        request.environment_count = 1;
    }
    aegir::spawn::Process process{};
    bool const spawned = spawner.spawn(request, account, process);
    if (spec.launcher) {
        /* The kit's namespace copy was auth's scratch: the service holds its own
         * now (specs/launch.md). */
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_kit_nmspace_slot,
                          aegir::bootstrap::cnode_bits());
    }
    if (!spawned) {
        write("      auth: FAIL spawning session.");
        write(spec.name);
        write(": ");
        write(spawner.problem());
        char const *const detail = spawner.detail();
        if (detail != nullptr && detail[0] != '\0') {
            write(" (");
            write(detail);
            write(")");
        }
        write("\n");
        return false;
    }
    /* The service is in the live set (specs/process.md): auth is its spawner,
     * and `name_buffer` is the `session.<name>` it stands as. */
    register_child(badge, name_buffer, name_at, spec.binary, spec.binary_length);
    if (supervision != nullptr) {
        *supervision = process.supervision;
    }
    return true;
}

/* A successful login starts a session (specs/auth.md, specs/session.md): the
 * smoke over the serial line, or the GUI session the manifest composes. The
 * badge is the user class bit, the row as the user id, and the serial counting
 * what the user has run (specs/authority.md); the session's services are the
 * manifest's, started in order as the session's user class. Everything the
 * spawn puts down -- the session's objects and the spawn's own staging -- is
 * retyped from the session pool and slotted past the mark, so the teardown
 * after the wait takes it all back; a windowed service's mapping kit (an untyped
 * for its page tables, its own VSpace root) is carved from the pool too. Then
 * the wait on the session's first long-lived child, and reclaim
 * (specs/auth.md's Session reclaim). */
void start_session(uint32_t user, bool bureau) noexcept
{
    uint32_t const serial = g_serials[user];
    uint64_t const badge = aegir::ipc::make_user_badge(user, serial);
    /* The badges auth mints directly for the session -- the session's own (the
     * first service shares it), each further service, and the launcher -- are
     * contiguous from `badge`, so reclaim releases that many. It grows as the
     * manifest's services and the launcher are minted; until then only the
     * session's own badge exists. */
    uint32_t direct_badges = 1;
    /* The home first: ensured and bound before the spawn, so the session
     * never sees a Home: that does not resolve (specs/auth.md's Homes). One
     * namespace exists for the session -- the services resolve through its
     * badged copy, not their own (specs/session.md) -- so this binds the one
     * set of aliases every program of the session shares. */
    ensure_home(user, badge);

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
        reclaim_session(badge, direct_badges, mark, scratch_mark, session_account);
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
        reclaim_session(badge, direct_badges, mark, scratch_mark, session_account);
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
    g_session_mem.adopt_slots_down(g_slots_first, g_slots_end - g_slots_first, 0,
                                   aegir::bootstrap::cnode_bits());
    g_session_mem.set_untyped_source(session_untyped_source, &g_session_mem);
    aegir::mem::Arena session_arena(g_session_mem, g_scratch, session_account);

    /* The spawner is the session's own: over the pool and the slots past
     * the mark, so nothing it puts down outlives the reclaim. */
    aegir::spawn::Initrd const initrd(reinterpret_cast<void const *>(g_binaries_address),
                                      g_binaries_bytes);
    aegir::spawn::Spawner spawner(g_session_mem, g_scratch, session_arena, initrd,
                                  g_asid_pool,
                                  static_cast<seL4_CPtr>(aegir::bootstrap::kSlotOwnCNode),
                                  aegir::bootstrap::cnode_bits());

    seL4_CPtr primary = 0;

    if (!bureau) {
        /* The smoke (specs/auth.md): the serial login's short-lived session,
         * two ports and no manifest. It signals its supervision as its last act,
         * so the wait below is also its exit. */
        seL4_Error fault_error = seL4_NoError;
        seL4_CPtr const fault = g_session_mem.alloc_object(
            seL4_EndpointObject, seL4_EndpointBits, session_account, &fault_error);
        if (fault == 0) {
            write("      auth: FAIL no fault endpoint for the session\n");
            reclaim_session(badge, direct_badges, mark, scratch_mark, session_account);
            return;
        }
        aegir::spawn::PortGrant ports[2] = {
            {aegir::log::kPortName, aegir::log::kPortNameLength,
             aegir::bootstrap::kSlotFirstDeclared, g_spawn_log,
             seL4_CapRights_new(1, 0, 0, 1), badge, 0},
            {aegir::nmspace::kPortName, aegir::nmspace::kPortNameLength,
             aegir::bootstrap::kSlotFirstDeclared + 1, g_spawn_nmspace,
             seL4_CapRights_new(1, 1, 0, 1), badge, 0},
        };
        static char const kSessionName[] = "session.smoke";
        static char const kSessionBinary[] = "aegir-session-smoke";
        static char const kHomeCwd[] = "Home:";
        aegir::spawn::Request request{};
        request.name = kSessionName;
        request.name_length = sizeof(kSessionName) - 1;
        request.binary = kSessionBinary;
        request.binary_length = sizeof(kSessionBinary) - 1;
        request.account = g_rows[user].account;
        request.account_length =
            field_length(g_rows[user].account, aegir::authdb::kAccountBytes);
        request.cwd = kHomeCwd;
        request.cwd_length = sizeof(kHomeCwd) - 1;
        request.priority = seL4_MaxPrio - 2;
        request.ports = ports;
        request.port_count = 2;
        request.fault_endpoint = fault;
        request.badge = badge;
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
            reclaim_session(badge, direct_badges, mark, scratch_mark, session_account);
            return;
        }
        /* The smoke is in the live set (specs/process.md): auth is its spawner. */
        register_child(badge, kSessionName, sizeof(kSessionName) - 1, kSessionBinary,
                       sizeof(kSessionBinary) - 1);
        primary = process.supervision;
    } else {
        /* The session's launch endpoint (specs/session.md): auth makes it, so no
         * client depends on a name a launcher chose. The session's other own
         * ports -- bureau.menu, datatypes.main -- are made on demand, when a
         * service's `owns` names them (own_endpoint), and their sources are
         * cleared here first so a session that owns none hands the launcher
         * none. */
        seL4_Error port_error = seL4_NoError;
        seL4_CPtr const launch_port = g_session_mem.alloc_object(
            seL4_EndpointObject, seL4_EndpointBits, session_account, &port_error);
        if (launch_port == 0) {
            write("      auth: FAIL no launch endpoint for the session\n");
            reclaim_session(badge, direct_badges, mark, scratch_mark, session_account);
            return;
        }
        g_spawn_bureau_menu = 0;
        g_spawn_datatypes = 0;

        /* The manifest (specs/session.md): the session's services as data. Read
         * it and resolve its services; an absent or malformed one leaves the
         * built-in terminal, so a login cannot come up empty and the user keeps
         * a way to recover. */
        uint32_t manifest_length = 0;
        char *const manifest_text = read_session_manifest(session_arena, &manifest_length);
        uint32_t service_count = 0;
        SessionService *services = nullptr;
        if (manifest_text == nullptr) {
            write("      auth: no session.manifest; the built-in session stands\n");
            services = static_cast<SessionService *>(
                session_arena.allocate(sizeof(SessionService)));
            if (services != nullptr) {
                default_terminal(&services[0]);
                service_count = 1;
            }
        } else {
            services = resolve_services(session_arena, session_account, manifest_text,
                                        manifest_length, &service_count);
        }
        if (services == nullptr) {
            write("      auth: FAIL no memory for the session's services\n");
            reclaim_session(badge, direct_badges, mark, scratch_mark, session_account);
            return;
        }
        /* The services run at `badge + i` and the launcher at
         * `badge + service_count`, all contiguous from the session's badge;
         * reclaim releases that many. A service that shares the session's badge
         * (i = 0) is already counted by the `1` when there are no services. */
        direct_badges = service_count + 1;

        /* The launcher's badge and the range it hands its children
         * (specs/launch.md): the serials past the session's own children, so no
         * two of a session's processes share one. */
        uint64_t const launcher_badge =
            aegir::ipc::make_user_badge(user, serial + service_count);
        uint32_t const range_base = serial + service_count + 1;

        for (uint32_t i = 0; i < service_count; ++i) {
            uint64_t const service_badge = aegir::ipc::make_user_badge(user, serial + i);
            seL4_CPtr supervision = 0;
            /* The namespace is the session's (`badge`), not the service's own:
             * every program of the session resolves through one badged copy
             * (specs/session.md), so only the session's aliases exist. */
            if (spawn_service(services[i], user, range_base, service_badge, badge,
                              launch_port, spawner, session_account, &supervision)) {
                write("      auth: session.");
                write(services[i].name);
                write(" is up\n");
            }
            if (primary == 0 && supervision != 0) {
                primary = supervision;
            }
        }

        /* The launcher (specs/launch.md): one per session, holding the spawn
         * kit and serving launch.session, whose owner half auth made above. It
         * draws nothing -- its runtime is for its own objects and the staging --
         * and it draws a nested shell's pool from mem.main on demand, so it is
         * given no shell pool. Its namespace is badged for the session, so it
         * and its children resolve every assign and volume the session has. Not
         * a manifest section: a session with no launcher could start nothing. */
        aegir::spawn::Kit launcher_kit{};
        launcher_kit.log = g_spawn_log;
        launcher_kit.console_gui = g_spawn_gui;
        launcher_kit.mem_main = g_spawn_mem;
        launcher_kit.asid_pool = g_asid_pool;
        launcher_kit.clock = g_spawn_clock;
        launcher_kit.timer = g_spawn_timer;
        launcher_kit.net_socket = g_spawn_net_socket;
        launcher_kit.net_control = g_spawn_net_control;
        launcher_kit.nmspace = g_kit_nmspace_slot;
        static char const kHomeCwd[] = "Home:";
        char const *const launcher_environment[1] = {
            badge_range_env(range_base, aegir::ipc::kSessionSerialStride - 3)};
        LauncherSpec launcher_spec{};
        launcher_spec.badge = launcher_badge;
        launcher_spec.namespace_badge = badge;
        launcher_spec.name = "session.launcher";
        launcher_spec.account = g_rows[user].account;
        launcher_spec.account_length =
            field_length(g_rows[user].account, aegir::authdb::kAccountBytes);
        launcher_spec.cwd = kHomeCwd;
        launcher_spec.environment = launcher_environment;
        launcher_spec.environment_count = 1;
        seL4_CPtr launcher_supervision = 0;
        if (spawn_launcher(g_session_mem, session_account, spawner, launcher_kit,
                           launcher_spec, launch_port, &launcher_supervision)) {
            write("      auth: the launcher is up\n");
        }
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_kit_nmspace_slot,
                          aegir::bootstrap::cnode_bits());
        if (primary == 0) {
            primary = launcher_supervision;
        }
    }

    write("      auth: ");
    write(g_rows[user].name, field_length(g_rows[user].name, aegir::authdb::kNameBytes));
    write(" authenticated, session started, badge ");
    aegir::debug_write_hex(badge);
    write("\n");

    g_serials[user] += aegir::ipc::kSessionSerialStride;

    /* The wait (specs/auth.md): a session that faults first leaves us here,
     * which is what waiting on it is for. The first long-lived child is the
     * session's representative -- a long-lived service signals nothing while it
     * lives, so the wait is the session's life; the smoke signals as its last
     * act, so there the wait is also the exit. */
    if (primary != 0) {
        seL4_Wait(primary, nullptr);
        write("      auth: session ready\n");
    }
    reclaim_session(badge, direct_badges, mark, scratch_mark, session_account);
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
    /* The boot session's class and library directories too, so a startup
     * command and the boot terminal's programs resolve them (specs/datatypes.md,
     * specs/libraries.md). */
    for (;;) {
        if (bind_name(kBootBadge, "DataTypes", 9, "Sys:DataTypes", 13)) {
            break;
        }
        seL4_Yield();
    }
    for (;;) {
        if (bind_name(kBootBadge, "LIBS", 4, "Sys:Libs", 8)) {
            break;
        }
        seL4_Yield();
    }

    /* The boot session's launcher (specs/launch.md): a Startup-Sequence runs
     * commands, and a command is started by the service that holds the spawn
     * kit -- so the boot session gets a launcher like a login's, serving the
     * endpoint auth makes here. */
    seL4_CPtr const launch_port = g_objects.alloc_object(
        seL4_EndpointObject, seL4_EndpointBits, account, &error);
    if (launch_port == 0) {
        write("      auth: FAIL no endpoint for the boot session's launcher\n");
        return false;
    }

    /* The boot terminal's kit, from the one first-class grant (specs/launch.md):
     * like a session's terminal, but badged with the boot badge and not a
     * launcher of launchers (it runs Startup-Sequence, no NEWSHELL). Its
     * namespace copies are badged for the boot session, so its shell resolves
     * Sys: and the boot aliases. */
    aegir::spawn::Kit boot_kit{};
    boot_kit.log = g_spawn_log;
    boot_kit.console_gui = g_spawn_gui;
    boot_kit.mem_main = g_spawn_mem;
    boot_kit.asid_pool = g_asid_pool;
    boot_kit.clock = g_spawn_clock;
    boot_kit.timer = g_spawn_timer;
    boot_kit.net_socket = g_spawn_net_socket;
    boot_kit.net_control = g_spawn_net_control;
    boot_kit.font_main = g_spawn_font;
    /* The boot terminal starts the Startup-Sequence's commands, so it registers
     * them, and the shell it starts (specs/process.md). */
    boot_kit.process_registry = g_spawn_process;

    aegir::spawn::Initrd const initrd(reinterpret_cast<void const *>(g_binaries_address),
                                      g_binaries_bytes);
    aegir::spawn::Spawner spawner(g_objects, g_scratch, arena, initrd, g_asid_pool,
                                  static_cast<seL4_CPtr>(aegir::bootstrap::kSlotOwnCNode),
                                  aegir::bootstrap::cnode_bits());

    /* The boot session's launcher: its namespace copy is minted for the boot
     * badge, so its commands resolve Sys:/C: the same way the shell does. */
    LauncherSpec launcher_spec{};
    launcher_spec.badge = kBootLauncherBadge;
    launcher_spec.namespace_badge = kBootBadge;
    launcher_spec.name = "system.launcher";
    launcher_spec.account = "system";
    launcher_spec.account_length = sizeof("system") - 1;
    launcher_spec.cwd = "Sys:";
    if (!spawn_launcher(g_objects, account, spawner, boot_kit, launcher_spec, launch_port)) {
        write("      auth: FAIL the boot session has no launcher\n");
        return false;
    }

    if (!mint_session_nmspace(g_kit_nmspace_slot, kBootBadge)) {
        write("      auth: FAIL no namespace copy for the boot terminal\n");
        return false;
    }
    boot_kit.nmspace = g_kit_nmspace_slot;
    aegir::spawn::Child boot_child{};
    boot_child.badge = kBootBadge;
    boot_child.runtime = terminal_untyped;
    boot_child.runtime_bits = kTerminalUntypedBits;
    boot_child.shell_pool = shell_pool;
    boot_child.shell_pool_bits = kShellPoolBits;
    boot_child.launcher = false;
    aegir::spawn::PortGrant ports[24];
    uint32_t port_count = aegir::spawn::launcher_ports(boot_kit, boot_child, ports, 24);
    /* The boot status endpoint is not part of the kit a launched program gets:
     * only the boot terminal's shell sends the outcome on it (specs/boot.md). */
    ports[port_count] = {"boot.status", 11,
                         aegir::bootstrap::kSlotFirstDeclared + port_count, boot_status,
                         seL4_AllRights, 0, 0};
    ++port_count;
    /* The boot launcher's caller half (specs/launch.md): the terminal passes it
     * to its shell, so a Startup-Sequence line can start a command -- what the
     * boot session could not do with built-ins alone. */
    ports[port_count] = {aegir::launch::kPortName, aegir::launch::kPortNameLength,
                         aegir::bootstrap::kSlotFirstDeclared + port_count, launch_port,
                         seL4_CapRights_new(1, 1, 0, 1), kBootBadge, 0};
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

    aegir::spawn::Process process{};
    if (!spawner.spawn(request, account, process)) {
        write("      auth: FAIL spawning the boot session: ");
        write(spawner.problem());
        write("\n");
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_kit_nmspace_slot,
                          aegir::bootstrap::cnode_bits());
        return true;
    }
    /* The boot terminal is in the live set (specs/process.md): auth is its
     * spawner. */
    register_child(kBootBadge, kName, sizeof(kName) - 1, kBinary, sizeof(kBinary) - 1);
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
        /* The font service (specs/fonts.md): the greeter draws its form in a
         * Sys:Fonts face. Absent when the director made no such port, and the
         * count keeps it out. */
        {"font.main", 9, aegir::bootstrap::kSlotFirstDeclared + 3, g_spawn_font,
         seL4_CapRights_new(1, 1, 0, 1), kGreeterBadge, 0},
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
    request.port_count = g_spawn_font != 0 ? 4 : 3;
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
    /* The greeter is in the live set (specs/process.md): auth is its spawner. */
    register_child(kGreeterBadge, kGreeterName, sizeof(kGreeterName) - 1, kGreeterBinary,
                   sizeof(kGreeterBinary) - 1);
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
    g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::cnode_bits()) - first_free, 0,
                          aegir::bootstrap::cnode_bits());
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
    /* The session manifest's resolve lands here (specs/session.md). */
    g_manifest_slot = g_objects.alloc_slot();
    /* A slot for the session's mem.main copy (specs/auth.md's Session
     * reclaim): minted fresh for each login's badge, below every session's
     * mark, so it is auth's own and not part of the session's range. */
    g_session_mem_call = g_objects.alloc_slot();
    g_kit_nmspace_slot = g_objects.alloc_slot();
    if (g_home_slot == 0 || g_manifest_slot == 0 || g_session_mem_call == 0 ||
        g_kit_nmspace_slot == 0) {
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
    /* bureau.menu is not adopted from director: a session's ports are the
     * session's, so auth makes it itself at login (start_session), the way it
     * makes launch.session. A boot session -- started before any login -- has
     * none, and its shell registers no menus (specs/session.md). */
    /* The font service (specs/fonts.md): present because the greeter and the
     * session entries need font.main. A boot without it keeps the built-in
     * Terminus, which is what every window drew before the service landed. */
    uint64_t spawn_font_slot = 0;
    if (aegir::bootstrap::capability("spawn:font.main", 15, &spawn_font_slot)) {
        g_spawn_font = static_cast<seL4_CPtr>(spawn_font_slot);
    }
    /* The process registry (specs/process.md): director grants the delegatable
     * copy because the session's terminal names process.registry, and auth
     * hands the launcher the source it mints each command's caller half from --
     * so a command's runtime registers itself and a Break can name it. */
    uint64_t spawn_process_slot = 0;
    if (aegir::bootstrap::capability("spawn:process.registry", 22, &spawn_process_slot)) {
        g_spawn_process = static_cast<seL4_CPtr>(spawn_process_slot);
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
    /* The network, optional the same way (specs/net.md): a session command may
     * use the stack, so the session authority names net.socket and net.control
     * and director grants the unbadged sources. A session without them runs
     * with no network, and a command that wants it says so. */
    uint64_t spawn_net_socket_slot = 0;
    if (aegir::bootstrap::capability("spawn:net.socket", 16, &spawn_net_socket_slot)) {
        g_spawn_net_socket = static_cast<seL4_CPtr>(spawn_net_socket_slot);
    }
    uint64_t spawn_net_control_slot = 0;
    if (aegir::bootstrap::capability("spawn:net.control", 17, &spawn_net_control_slot)) {
        g_spawn_net_control = static_cast<seL4_CPtr>(spawn_net_control_slot);
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
