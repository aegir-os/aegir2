/*
 * The spawn kit's grants, built (specs/launch.md, specs/authority.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One place decides the slot layout, the names and the mint/copy of every
 * capability a child is handed, so no spawner reassembles it.
 */

#include <aegir/spawn/kit.h>
#include <aegir/bootstrap.h>
#include <aegir/datatypes.h>

namespace aegir::spawn {

namespace {

/* The font service's port, as the clients name it (aegir/font.h): spelled here
 * so the kit links no protocol it does not otherwise hold. The length is the
 * name's own, not `bureau.menu`'s -- a wrong one registers the capability under
 * a name no client finds (aegir-bootstrap's `capability` compares the length). */
constexpr char kFontMainName[] = "font.main";
constexpr uint32_t kFontMainNameLength = sizeof(kFontMainName) - 1;

/* Append one grant at `index`, unless the caller's array is already full. The
 * slot is the layout's, from kSlotFirstDeclared; the caller sizes the array
 * from the builders' maxima (a command's 7, a launcher's 12, a shell's 5). */
bool put(PortGrant *out, uint32_t capacity, uint32_t index, char const *name,
         uint32_t name_length, seL4_CPtr cap, seL4_CapRights_t rights, uint64_t badge,
         uint32_t bits, bool move, bool copy) noexcept
{
    if (index >= capacity) {
        return false;
    }
    out[index] = PortGrant{name, name_length, aegir::bootstrap::kSlotFirstDeclared + index,
                           cap, rights, badge, bits, move, copy};
    return true;
}

/* The four grants every windowed child starts with, in order: its log, the
 * session's namespace by copy, its own badged console.gui, and the runtime its
 * heap and page tables come from. A kind-3 launcher appends its spawn kit; a
 * kind-2 windowed program stops here. */
uint32_t identity_ports(Kit const &kit, Child const &child, PortGrant *out,
                        uint32_t capacity)
{
    uint32_t n = 0;
    (void)put(out, capacity, n++, aegir::log::kPortName, aegir::log::kPortNameLength,
              kit.log, seL4_CapRights_new(1, 0, 0, 1), child.badge, 0, false, false);
    (void)put(out, capacity, n++, aegir::nmspace::kPortName,
              aegir::nmspace::kPortNameLength, kit.nmspace, seL4_CapRights_new(1, 1, 0, 1),
              0, 0, false, true);
    (void)put(out, capacity, n++, aegir::console::kPortName,
              aegir::console::kPortNameLength, kit.console_gui,
              seL4_CapRights_new(1, 1, 0, 1), child.badge, 0, false, false);
    (void)put(out, capacity, n++, "untyped", 7, child.runtime, seL4_AllRights, 0,
              child.runtime_bits, false, false);
    return n;
}

}  // namespace

uint32_t command_ports(Kit const &kit, Child const &child, PortGrant *out, uint32_t capacity)
{
    uint32_t n = 0;
    (void)put(out, capacity, n++, aegir::console::kStreamPortName,
              aegir::console::kStreamPortNameLength, kit.stream,
              seL4_CapRights_new(1, 1, 0, 1), child.stream_badge, 0, false,
              child.stream_copy);
    (void)put(out, capacity, n++, "untyped", 7, child.runtime, seL4_AllRights, 0,
              child.runtime_bits, false, false);
    /* The session's namespace, by copy: it already carries the session's
     * identity, which is what makes Home: and ENV: resolve (specs/dos.md). */
    (void)put(out, capacity, n++, aegir::nmspace::kPortName,
              aegir::nmspace::kPortNameLength, kit.nmspace,
              seL4_CapRights_new(1, 1, 0, 1), 0, 0, false, true);
    /* The command's own badged memory copy, by copy: the badge is already on
     * it, and its runtime grows through it under its own id (specs/memory.md). */
    (void)put(out, capacity, n++, aegir::memory::kPortName,
              aegir::memory::kPortNameLength, child.mem, seL4_CapRights_new(1, 1, 0, 1),
              0, 0, false, true);
    /* Its own console, when the launcher has one to give: a program opens a
     * window whenever it wants to use the GUI, rather than the launcher
     * deciding before it runs (specs/launch.md). Optional -- a launcher with
     * no unbadged console.gui hands none, and its programs run console-only. */
    if (kit.console_gui != 0) {
        (void)put(out, capacity, n++, aegir::console::kPortName,
                  aegir::console::kPortNameLength, kit.console_gui,
                  seL4_CapRights_new(1, 1, 0, 1), child.badge, 0, false, false);
    }
    /* The caller's bureau.menu (specs/workbench.md): a windowed program registers
     * the menus the screen bar shows while it is the active window -- the
     * Workbench model, where before only the demo, a boot service, could. Minted
     * with the child's own badge, so the bureau learns who called; the source is
     * the unbadged delegate (specs/launch.md). */
    if (kit.bureau_menu != 0) {
        (void)put(out, capacity, n++, "bureau.menu", 11, kit.bureau_menu,
                  seL4_CapRights_new(1, 1, 0, 1), child.badge, 0, false, false);
    }
    /* The caller's font.main (specs/fonts.md): the font service, so a program
     * draws a Sys:Fonts face the toolkit cannot parse itself -- an OpenType one.
     * Minted with the child's own badge; the source is the unbadged delegate. */
    if (kit.font_main != 0) {
        (void)put(out, capacity, n++, kFontMainName, kFontMainNameLength, kit.font_main,
                  seL4_CapRights_new(1, 1, 0, 1), child.badge, 0, false, false);
    }
    /* The session launcher's caller half (specs/launch.md, specs/datatypes.md):
     * a command may ask the launcher to serve-launch a class, so any program can
     * use a resource library, not only a launching program. Copied, not minted:
     * the launcher's half is already badged, and the kernel refuses to re-badge
     * an endpoint cap (updateCapData returns null when the badge is nonzero), so
     * a mint fails to install -- and the launcher ignores the call's badge and
     * attributes a command from its own range, so a copy loses nothing. An
     * output view is a command, so it shares this one grant -- which is why it
     * lives here, not in output_ports, where a second entry under the same name
     * would shadow it. */
    if (kit.launch != 0) {
        (void)put(out, capacity, n++, aegir::launch::kPortName,
                  aegir::launch::kPortNameLength, kit.launch,
                  seL4_CapRights_new(1, 1, 0, 1), 0, 0, false, true);
    }
    if (kit.clock != 0) {
        (void)put(out, capacity, n++, aegir::clock::kPortName,
                  aegir::clock::kPortNameLength, kit.clock, seL4_CapRights_new(1, 0, 0, 1),
                  0, 0, false, false);
    }
    if (kit.timer != 0) {
        (void)put(out, capacity, n++, aegir::timer::kPortName,
                  aegir::timer::kPortNameLength, kit.timer, seL4_CapRights_new(1, 0, 0, 1),
                  0, 0, false, false);
    }
    return n;
}

uint32_t serve_ports(Kit const &kit, Child const &child, PortGrant *out, uint32_t capacity)
{
    uint32_t n = 0;
    /* The caller's class port, under its own name: the class serves it, so it
     * is the receiver, and the caller keeps the other half. It is not the
     * con.stream, so the class's fd 1/2 stay the debug serial. */
    (void)put(out, capacity, n++, aegir::datatypes::kClassPortName,
              aegir::datatypes::kClassPortNameLength, kit.stream,
              seL4_CapRights_new(1, 1, 0, 1), child.stream_badge, 0, false,
              child.stream_copy);
    (void)put(out, capacity, n++, "untyped", 7, child.runtime, seL4_AllRights, 0,
              child.runtime_bits, false, false);
    /* The session's namespace, by copy: the class reads the caller's file
     * through it, on the caller's badge (specs/datatypes.md). */
    (void)put(out, capacity, n++, aegir::nmspace::kPortName,
              aegir::nmspace::kPortNameLength, kit.nmspace,
              seL4_CapRights_new(1, 1, 0, 1), 0, 0, false, true);
    /* Its own badged memory copy, by copy: the badge is already on it. */
    (void)put(out, capacity, n++, aegir::memory::kPortName,
              aegir::memory::kPortNameLength, child.mem, seL4_CapRights_new(1, 1, 0, 1),
              0, 0, false, true);
    if (kit.clock != 0) {
        (void)put(out, capacity, n++, aegir::clock::kPortName,
                  aegir::clock::kPortNameLength, kit.clock, seL4_CapRights_new(1, 0, 0, 1),
                  0, 0, false, false);
    }
    if (kit.timer != 0) {
        (void)put(out, capacity, n++, aegir::timer::kPortName,
                  aegir::timer::kPortNameLength, kit.timer, seL4_CapRights_new(1, 0, 0, 1),
                  0, 0, false, false);
    }
    return n;
}

uint32_t output_ports(Kit const &kit, Child const &child, PortGrant *out, uint32_t capacity)
{
    /* A view is a command that serves con.stream instead of writing it, so it
     * inherits the command's `launch.session` caller half -- which is what it
     * releases each command through (kMethodRelease). Nothing is added here, so
     * the grant is not duplicated under one name. */
    return command_ports(kit, child, out, capacity);
}

uint32_t launcher_ports(Kit const &kit, Child const &child, PortGrant *out, uint32_t capacity)
{
    uint32_t n = identity_ports(kit, child, out, capacity);
    /* Without a memory service there is no launcher kit: the child runs and
     * says it has no spawner (auth's degraded boot). */
    if (kit.mem_main == 0) {
        return n;
    }
    (void)put(out, capacity, n++, "spawn:mem.main", 14, kit.mem_main,
              seL4_CapRights_new(1, 1, 0, 1), 0, 0, false, false);
    (void)put(out, capacity, n++, "asid-pool", 9, kit.asid_pool, seL4_AllRights, 0, 0,
              false, false);
    (void)put(out, capacity, n++, "spawn:log.main", 14, kit.log,
              seL4_CapRights_new(1, 0, 0, 1), 0, 0, false, false);
    (void)put(out, capacity, n++, "shell:vfs.namespace", 19, kit.nmspace,
              seL4_CapRights_new(1, 1, 0, 1), 0, 0, false, true);
    /* A shell pool only when the child was given one: a launcher draws a
     * nested shell's pool from mem.main on demand and has none of its own. */
    if (child.shell_pool != 0) {
        (void)put(out, capacity, n++, "shell-pool", 10, child.shell_pool, seL4_AllRights, 0,
                  child.shell_pool_bits, false, false);
    }
    if (kit.clock != 0) {
        (void)put(out, capacity, n++, "spawn:clock.main", 16, kit.clock,
                  seL4_CapRights_new(1, 0, 0, 1), 0, 0, false, false);
    }
    if (kit.timer != 0) {
        (void)put(out, capacity, n++, "spawn:timer.main", 16, kit.timer,
                  seL4_CapRights_new(1, 0, 0, 1), 0, 0, false, false);
    }
    /* A launcher is also given the unbadged console, so its own children can
     * mint their own attach. A plain launching child -- one that runs but does
     * not launch -- stops above. */
    if (child.launcher) {
        (void)put(out, capacity, n++, "spawn:console.gui", 17, kit.console_gui,
                  seL4_CapRights_new(1, 1, 0, 1), 0, 0, false, false);
    }
    /* The unbadged font.main (specs/fonts.md): a launching child mints its own
     * commands a caller half from it, and a windowed child calls the service
     * directly -- the launcher and the session's terminal both stop here. */
    if (kit.font_main != 0) {
        (void)put(out, capacity, n++, kFontMainName, kFontMainNameLength, kit.font_main,
                  seL4_CapRights_new(1, 1, 0, 1), 0, 0, false, false);
    }
    return n;
}

uint32_t shell_ports(Kit const &kit, Child const &child, PortGrant *out, uint32_t capacity)
{
    uint32_t n = 0;
    (void)put(out, capacity, n++, aegir::console::kStreamPortName,
              aegir::console::kStreamPortNameLength, kit.stream,
              seL4_CapRights_new(1, 1, 0, 1), child.badge, 0, false, false);
    (void)put(out, capacity, n++, aegir::log::kPortName, aegir::log::kPortNameLength,
              kit.log, seL4_CapRights_new(1, 0, 0, 1), 0, 0, false, false);
    /* Moved, not copied: the shell is spawned once, so the one use is the one
     * move, and the terminal keeps its own namespace for the commands it
     * copies. */
    (void)put(out, capacity, n++, aegir::nmspace::kPortName,
              aegir::nmspace::kPortNameLength, kit.shell_nmspace,
              seL4_CapRights_new(1, 1, 0, 1), 0, 0, true, false);
    (void)put(out, capacity, n++, "untyped", 7, child.runtime, seL4_AllRights, 0,
              child.runtime_bits, false, false);
    /* The launcher's caller half (specs/launch.md): copied, not minted -- it is
     * already badged, and a badged endpoint cap cannot be minted again. A
     * process with no launcher (the boot session's terminal) is granted none,
     * so its shell's launches fail rather than guess at a port. */
    if (kit.launch != 0) {
        (void)put(out, capacity, n++, aegir::launch::kPortName,
                  aegir::launch::kPortNameLength, kit.launch,
                  seL4_CapRights_new(1, 1, 0, 1), 0, 0, false, true);
    }
    return n;
}

}  // namespace aegir::spawn
