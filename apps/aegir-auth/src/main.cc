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
#include <aegir/nmspace.h>
#include <aegir/spawn/initrd.h>
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

/* The greeter's kit (specs/console.md's login arc): the delegatable copies
 * its spawn takes, and the console caller half that is auth's own -- a login
 * through the greeter ends with auth reaping the greeter's windows and
 * slice, and the reap call is this half's. The badge is from auth's system
 * children range (specs/authority.md). */
seL4_CPtr g_spawn_gui = 0;
seL4_CPtr g_spawn_login = 0;
aegir::ipc::Consumer g_gui;
constexpr uint64_t kGreeterBadge = 768;
bool g_greeter_up = false;
/* The greeter's supervision notification: signalled twice -- the form is on
 * the screen (start_greeter's wait), and the accepted login's exit (the
 * login handler's wait, before the reap). */
seL4_CPtr g_greeter_supervision = 0;

/* The namespace as auth speaks it, and the slot a home resolve's capability
 * lands in -- one slot, deleted after each use, so a login does not spend
 * what the next one needs. */
aegir::ipc::Consumer g_nmspace;
seL4_CPtr g_home_slot = 0;

/* The session pool (specs/auth.md's Session reclaim): one untyped, carved
 * out of the delegation at boot and kept. A session's objects are retyped
 * from it, so an exit's one revoke frees it whole and the next login
 * reuses it -- the wait serializes sessions, so one pool is enough. Its
 * size is a starting grant: the reclaim log line says what a session
 * charged, and the grant grows when that says so. */
constexpr uint32_t kSessionPoolBits = 20; /* 1 MiB of the 4 MiB delegation */
seL4_CPtr g_session_pool = 0;
uint64_t g_session_pool_physical = 0;

/* The allocator over the session pool: static, because the untyped table
 * inside one is far larger than a service's stack -- and reset each login,
 * because the revoke made the pool whole again and last session's split
 * records belong to capabilities that no longer exist. */
aegir::mem::Allocator g_session_mem(nullptr);

/* What a session spawn needs, kept from the bootstrap block: the initrd's
 * bytes (the binary is looked up by name), the ASID pool the address space
 * comes from, and the end of our slot range -- a session's slots run from
 * the login's mark to it. */
uint64_t g_binaries_address = 0;
uint32_t g_binaries_bytes = 0;
seL4_CPtr g_asid_pool = 0;
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
            uint64_t mout[aegir::nmspace::kPathMax / 8 + 1];
            uint32_t const mout_words = aegir::nmspace::pack_string(
                mout, rest, rest_length, aegir::nmspace::kPathMax);
            uint64_t min[1];
            aegir::ipc::WordsReply const answered = volume.call_words(
                aegir::volume::kMethodMkdir, mout, mout_words, min, 1);
            made = answered.error == 0 && answered.count == 1 && min[0] == 1;
            seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_home_slot,
                              aegir::bootstrap::kCNodeBits);
        }
    }
    if (!made) {
        write("      auth: the home would not be made -- the session starts "
              "without one\n");
    }

    uint64_t out[1 + aegir::nmspace::kNameMax / 8 + 1 + aegir::nmspace::kPathMax / 8 + 1];
    out[0] = badge;
    uint32_t out_words = 1;
    out_words += aegir::nmspace::pack_string(out + out_words, "Home", 4,
                                             aegir::nmspace::kNameMax);
    out_words += aegir::nmspace::pack_string(out + out_words, home, home_length,
                                             aegir::nmspace::kPathMax);
    uint64_t in[1];
    aegir::ipc::WordsReply const bound =
        g_nmspace.call_words(aegir::nmspace::kMethodBind, out, out_words, in, 1);
    if (bound.error != 0 || bound.count != 1 || in[0] != 1) {
        write("      auth: FAIL the Home: bind was refused\n");
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
                          aegir::bootstrap::kCNodeBits);
    }
    uint64_t const badge_word = badge;
    uint64_t uin[1];
    aegir::ipc::WordsReply const unbound_reply =
        g_nmspace.call_words(aegir::nmspace::kMethodUnbind, &badge_word, 1, uin, 1);
    uint64_t const unbound =
        (unbound_reply.error == 0 && unbound_reply.count == 1) ? uin[0] : 0;

    /* The revoke is the memory's way back: every object the session was --
     * CSpace, TCB, VSpace, frames, the spawn's staging -- was retyped from
     * the pool, and with the CSpace go the minted port copies it held
     * (specs/authority.md's retained-copy path). The kernel unmaps a mapped
     * frame when the cap goes (finaliseCap), so the staging's scratch-window
     * pages are already unmapped here; the window's cursor just needs to be
     * told. The pool stands free whole for the next login, and the slots
     * past the mark are empty, so the cursor returns to it. */
    seL4_CNode_Revoke(aegir::bootstrap::kSlotOwnCNode, g_session_pool,
                      aegir::bootstrap::kCNodeBits);
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
    uint64_t const badge =
        aegir::ipc::make_user_badge(user, g_serials[user]);
    /* The home first: ensured and bound before the spawn, so the session
     * never sees a Home: that does not resolve (specs/auth.md's Homes). */
    ensure_home(user, badge);

    seL4_CPtr const mark = g_objects.slot_mark();
    /* The scratch window's own mark: the spawn stages the child's block and
     * stack through it, and those pages are the session's to hand back --
     * the rewind in reclaim_session is what keeps one login's staging from
     * climbing the window until a table allocation collides with the
     * session's own slots. */
    uintptr_t const scratch_mark = g_scratch.next();
    aegir::mem::Account session_account{"session", 0, 0, 0};
    g_session_mem.reset();
    if (!g_session_mem.adopt_untyped(g_session_pool, kSessionPoolBits,
                                     g_session_pool_physical)) {
        write("      auth: FAIL the session pool would not be adopted\n");
        reclaim_session(badge, mark, scratch_mark, session_account);
        return;
    }
    g_session_mem.adopt_slots(mark, g_slots_end - mark, 0);
    aegir::mem::Arena session_arena(g_session_mem, g_scratch, session_account);

    seL4_Error fault_error = seL4_NoError;
    seL4_CPtr const fault = g_session_mem.alloc_object(seL4_EndpointObject, seL4_EndpointBits,
                                                       session_account, &fault_error);
    if (fault == 0) {
        write("      auth: FAIL no fault endpoint for the session\n");
        reclaim_session(badge, mark, scratch_mark, session_account);
        return;
    }
    /* The bureau's mapping kit: 256 KiB of the pool for the page tables its
     * slice mapping is retyped from, and the grant travels with its size,
     * because a service cannot ask the kernel how large an untyped is. */
    constexpr uint32_t kBureauUntypedBits = 18;
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
    aegir::spawn::PortGrant const ports[] = {
        {aegir::log::kPortName, aegir::log::kPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared, g_spawn_log, seL4_CapRights_new(1, 0, 0, 1),
         badge, 0},
        /* The namespace, with Grant: a resolve's answer carries a
         * capability, and a cap crosses only between halves that may grant
         * (specs/services.md). */
        {aegir::nmspace::kPortName, aegir::nmspace::kPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared + 1, g_spawn_nmspace,
         seL4_CapRights_new(1, 1, 0, 1), badge, 0},
        /* The bureau's two extras (dead entries for the smoke -- the count
         * says which are live): the console, with Grant, because frame and
         * listen answers carry capabilities; and the untyped, whole. */
        {aegir::console::kPortName, aegir::console::kPortNameLength,
         aegir::bootstrap::kSlotFirstDeclared + 2, g_spawn_gui,
         seL4_CapRights_new(1, 1, 0, 1), badge, 0},
        {"untyped", 7, aegir::bootstrap::kSlotFirstDeclared + 3, bureau_untyped,
         seL4_AllRights, 0, kBureauUntypedBits},
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
    request.priority = seL4_MaxPrio - 2;
    request.ports = ports;
    request.port_count = bureau ? 4 : 2;
    request.fault_endpoint = fault;
    request.badge = badge;

    /* The spawner is the session's own: over the pool and the slots past
     * the mark, so nothing it puts down outlives the reclaim. */
    aegir::spawn::Initrd const initrd(reinterpret_cast<void const *>(g_binaries_address),
                                      g_binaries_bytes);
    aegir::spawn::Spawner spawner(g_session_mem, g_scratch, session_arena, initrd,
                                  g_asid_pool,
                                  static_cast<seL4_CPtr>(aegir::bootstrap::kSlotOwnCNode),
                                  aegir::bootstrap::kCNodeBits);
    aegir::spawn::Process process{};
    if (!spawner.spawn(request, session_account, process)) {
        write("      auth: FAIL spawning the session: ");
        write(spawner.problem());
        write("\n");
        reclaim_session(badge, mark, scratch_mark, session_account);
        return;
    }
    write("      auth: ");
    write(g_rows[user].name, field_length(g_rows[user].name, aegir::authdb::kNameBytes));
    write(" authenticated, session started, badge ");
    aegir::debug_write_hex(badge);
    write("\n");
    ++g_serials[user];

    /* The ready, waited on the way the partition manager waits for a
     * filesystem's: a session that faults first leaves us here, which is
     * what waiting on it is for. While sessions are short-lived the ready
     * is also the exit -- the smoke signals as its last act -- so the wait
     * returning is how we know the session died (specs/auth.md). */
    seL4_Wait(process.supervision, nullptr);
    write("      auth: session ready\n");
    reclaim_session(badge, mark, scratch_mark, session_account);
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
                                  aegir::bootstrap::kCNodeBits);
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
    g_objects.adopt_slots(first_free, (1u << 10) - first_free, 0);
    g_slots_end = 1u << 10;
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

    /* The session pool (specs/auth.md's Session reclaim): carved once and
     * kept -- a session's objects are retyped from it, and an exit's one
     * revoke frees it whole for the next login. */
    seL4_Error pool_error = seL4_NoError;
    g_session_pool = g_objects.carve_untyped(kSessionPoolBits, g_account, &pool_error,
                                             &g_session_pool_physical);
    if (g_session_pool == 0) {
        write("      auth: FAIL no session pool -- logins will not start "
              "sessions\n");
    }

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
    aegir::spawn::Initrd const initrd(reinterpret_cast<void const *>(g_binaries_address),
                                      g_binaries_bytes);
    bool const can_spawn = g_session_pool != 0 && kit_complete && initrd.valid();
    if (!can_spawn) {
        write("      auth: no pool, delegatable ports, or initrd -- "
              "logins will not start sessions\n");
    }

    write("      auth: ");
    aegir::debug_write_unsigned(g_users);
    write(g_users == 1 ? " user, serving auth.login\n" : " users, serving auth.login\n");
    start_greeter(arena);
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
