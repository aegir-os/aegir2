/*
 * aegir-bureau: the session a greeter login starts -- the screen, handed over.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The bureau is the Amiga screen as a shape of window (specs/bureau.md): one
 * full-screen window, always in backdrop mode. This arc makes it the
 * Workbench (specs/workbench.md): it stays alive and owns the screen title
 * bar across the top, with the always-visible menus the bar holds. Its
 * content is a Desktop -- the backdrop, the bar, and the menus -- and a menu
 * item's action prints a cue.
 *
 * It asks the console for the screen's size, so a full-screen window matches
 * the mode the driver settled on. It does not halt: the console owns the
 * slice, so the backdrop stands whether or not the bureau is scheduled, and a
 * live bureau is what can answer a menu.
 */

#include <aegir/bootstrap.h>
#include <aegir/bureau/desktop.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/window.h>
#include <sel4/sel4.h>
#include <memory>
#include <vector>

namespace {

void write(char const *text)
{
    aegir::debug_write(text);
}

/* The bureau's menus (specs/workbench.md): its own, until the bureau.menu
 * server lets an app register its own. */
std::vector<aegir::bureau::Desktop::Menu> bureau_menus()
{
    using MenuItem = aegir::bureau::Desktop::MenuItem;
    using Menu = aegir::bureau::Desktop::Menu;

    Menu bureau;
    bureau.title = U"Bureau";
    bureau.items = {
        {1, U"About Aegir", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::NONE, {}},
        {2, U"Open...", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::DISABLED, {}},
        {0, U"", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::SEPARATOR, {}},
        {3, U"Quit", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::NONE, {}},
    };

    Menu window;
    window.title = U"Window";
    window.items = {
        {4, U"Clean Up", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::NONE, {}},
        {5, U"Open Windows...", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::DISABLED, {}},
    };

    Menu icons;
    icons.title = U"Icons";
    icons.items = {
        {6, U"Show", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::NONE, {}},
        {7, U"Hide", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::NONE, {}},
    };

    return {std::move(bureau), std::move(window), std::move(icons)};
}

}  // namespace

int main(int argc, char *argv[])
{
    using namespace aegir::trinket;

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    Application &app = Application::create(argc, argv);

    aegir::ipc::Consumer const gui = aegir::ipc::Consumer::find(
        aegir::console::kPortName, aegir::console::kPortNameLength);
    if (!gui.valid()) {
        write("  bureau: FAIL no console.gui\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    app.set_gui_port(gui);

    /* The screen's size is the console's to say (specs/bureau.md). */
    uint64_t width = 0;
    uint64_t height = 0;
    if (!aegir::console::info(gui, &width, &height)) {
        write("  bureau: FAIL the console would not say the screen's size\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    Window window(app);
    window.set_rect({0, 0, static_cast<int>(width), static_cast<int>(height)});
    /* Backdrop mode: the bottom of the z-order, beneath every window that
     * comes later (specs/console.md's focus model). */
    window.set_decorated(false);

    auto desktop = std::make_unique<aegir::bureau::Desktop>();
    desktop->set_menus(bureau_menus());
    desktop->on_menu_opened = [](int) { write("  bureau: menu\n"); };
    desktop->on_action = [&](uint32_t action_id) {
        if (action_id == 1) {
            write("  bureau: Aegir, the Workbench\n");
        } else if (action_id == 3) {
            write("  bureau: quit\n");
            aegir::halt();
        } else if (action_id == 4) {
            write("  bureau: clean up\n");
        } else if (action_id == 6) {
            write("  bureau: icons shown\n");
        } else if (action_id == 7) {
            write("  bureau: icons hidden\n");
        }
    };
    window.set_content(std::move(desktop));
    window.show();

    app.on_started = [&]() {
        write("  bureau: the screen is yours\n");
        if (log.valid()) {
            (void)log.call(aegir::log::kMethodEvent,
                           static_cast<uint64_t>(aegir::log::Event::Ready));
        }
        /* No supervision signal: auth reads a session's first supervision as
         * its exit and reclaims it (apps/aegir-auth), which a short-lived
         * smoke can be and the desktop cannot. A long-lived session's ready
         * and exit are separate, and the split is the supervisor arc's; until
         * then auth waits here for the exit this never sends, which is what a
         * session that is the desktop means. */
    };

    return app.exec();
}
