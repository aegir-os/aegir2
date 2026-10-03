/*
 * The spawn kit as a first-class grant (specs/launch.md, specs/authority.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A launcher starts programs out of authority it was delegated: a memory
 * service, an ASID pool, an unbadged console, the session's namespace, a log,
 * a clock and a timer, and the endpoints it made for its own commands. What a
 * child is handed from that is not each launcher's private list -- it is a
 * grant with a fixed shape, built here once. auth builds the session's terminal
 * with it; the terminal builds its commands, its shell and nested terminals
 * with it; a session launcher service one day uses the same call.
 *
 * The mint/copy decision is the whole subtlety, and it is a property of the
 * cap, not of the child: an *unbadged* source (log, console, mem.main) is
 * minted with the child's badge, and a *badged* one (the session's namespace,
 * which must carry the session's identity so Home: and ENV: resolve) is
 * copied. A launcher fills a Kit with caps already in the form the grant
 * needs, and the grant is then the same code for everyone.
 */

#ifndef AEGIR_SPAWN_KIT_H
#define AEGIR_SPAWN_KIT_H

#include <aegir/clock.h>
#include <aegir/console.h>
#include <aegir/console_stream.h>
#include <aegir/launch.h>
#include <aegir/log.h>
#include <aegir/memory.h>
#include <aegir/nmspace.h>
#include <aegir/spawn/process.h>
#include <aegir/timer.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::spawn {

/** What a launcher holds to start the session's programs. The unbadged
 *  members are the sources a child's own caps are minted from; the two
 *  namespace members are badged for the session (a child is handed a copy, so
 *  it keeps the session's identity). `stream` and `launch` are the launcher's
 *  own endpoints, not delegates. */
struct Kit {
    /* Unbadged delegates. */
    seL4_CPtr log = 0;
    seL4_CPtr console_gui = 0;
    /* The caller half of the bureau.menu port (specs/workbench.md), so a
     * *launched* program can register the menus the screen bar shows while it
     * is active -- the Workbench model, where the demo, a boot service, was
     * until now the only client. Optional: a launcher with none hands its
     * commands none, and they run without menus. */
    seL4_CPtr bureau_menu = 0;
    /* The caller half of font.main, the font service (specs/fonts.md), so a
     * launched program can draw a face from Sys:Fonts that the toolkit cannot
     * parse itself -- a TrueType or OpenType one. Optional: a launcher with none
     * hands its commands none, and they keep the built-in Terminus. */
    seL4_CPtr font_main = 0;
    /* The unbadged datatypes.main source (specs/datatypes.md): the session's
     * broker, so a launched program asks it to open a file rather than starting
     * a class itself. Optional: a launcher handed none hands its commands none,
     * and a client falls back to starting the class through its launcher
     * (specs/datatypes.md's client API). */
    seL4_CPtr datatypes = 0;
    seL4_CPtr mem_main = 0;
    seL4_CPtr asid_pool = 0;
    seL4_CPtr clock = 0; /* optional */
    seL4_CPtr timer = 0; /* optional */
    /* The session's namespace, badged for the session: copied to a child's own
     * slot, and (shell_nmspace) moved to the shell. Two caps, because the one a
     * shell takes is moved and a moved cap cannot also be copied. */
    seL4_CPtr nmspace = 0;
    seL4_CPtr shell_nmspace = 0;
    /* The launcher's own: the stream its commands share, and the caller half of
     * launch.session it hands a shell or a nested terminal -- copied, never
     * minted, because it is already badged. A process that launches nothing
     * (the boot shell's terminal) has none, and the grant is skipped. */
    seL4_CPtr stream = 0;
    seL4_CPtr launch = 0;
    /* The boot session's status endpoint, when this is the boot terminal. */
    seL4_CPtr boot_status = 0;
};

/** The child a grant is built for. `badge` identifies the child; `runtime` is
 *  the untyped its heap and page tables come from (for a shell, its shell
 *  pool); `mem` is a command's own badged mem.main copy. */
struct Child {
    uint64_t badge = 0;
    seL4_CPtr runtime = 0;
    uint32_t runtime_bits = 0;
    seL4_CPtr shell_pool = 0; /* kind 3 */
    uint32_t shell_pool_bits = 0;
    seL4_CPtr mem = 0;            /* kind 1 */
    uint64_t stream_badge = 0;    /* kind 1: the con.stream set it joins */
    /* The stream cap is already badged -- a launcher's own con.stream for a
     * command it starts, minted onto the caller's copy -- so it must be copied
     * rather than minted (a badged cap cannot be minted again). */
    bool stream_copy = false;
    bool launcher = false;        /* kind 3: also the unbadged launcher console */
};

/** A command (kind 1): the launcher's stream, its runtime, the session's
 *  namespace by copy, its own memory copy, the clock and timer
 *  when the launcher has them, and -- when the launcher holds one -- its own
 *  badged console.gui, so a program opens a window whenever it wants one
 *  rather than being classified before it runs (specs/launch.md). */
uint32_t command_ports(Kit const &kit, Child const &child, PortGrant *out, uint32_t capacity);

/** A class (specs/datatypes.md): the caller's serve port under
 *  `datatypes.class`, its runtime, the session's namespace by copy, its own
 *  memory copy, and the clock and timer when the launcher has them. The class
 *  reads the file through the namespace and serves its frame on the port; it is
 *  given no window and no console stream, so its fd 1/2 stay the debug serial.
 *  `kit.stream` is the caller's serve cap and `child.stream_copy` must be true. */
uint32_t serve_ports(Kit const &kit, Child const &child, PortGrant *out, uint32_t capacity);

/** An output view (specs/launch.md): the launcher's command grant, with the
 *  con.stream cap as a *copy* -- the view owns and serves the endpoint a
 *  stream-less caller's command writes to, so it is the receiver, not a caller
 *  -- plus the launcher's `launch.session` caller half, so the view can release
 *  each command whose exit it reports (the command's badge arrives in the exit
 *  report). `kit.stream` is the view's own endpoint; `child.stream_copy` must be
 *  true. Everything else is a command's: its runtime, its own badged memory, the
 *  session's namespace, its console.gui, the clock and the timer. */
uint32_t output_ports(Kit const &kit, Child const &child, PortGrant *out, uint32_t capacity);

/** A launching program (kind 3): its own console and identity, its runtime, and
 *  the unbadged sources it will hand its own children. When the launcher holds
 *  no memory service -- the degraded boot -- only the first four entries are
 *  emitted, and the child runs without a spawner. */
uint32_t launcher_ports(Kit const &kit, Child const &child, PortGrant *out, uint32_t capacity);

/** The launcher's own shell: its stream and launch port, the log, the session's
 *  namespace *moved* (the shell is spawned once), and the shell pool. The boot
 *  terminal's status endpoint is appended by the caller, because it is not part
 *  of the kit a launched program gets. */
uint32_t shell_ports(Kit const &kit, Child const &child, PortGrant *out, uint32_t capacity);

}  // namespace aegir::spawn

#endif  // AEGIR_SPAWN_KIT_H
