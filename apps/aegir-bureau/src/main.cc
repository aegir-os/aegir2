/*
 * aegir-bureau: the session a greeter login starts -- the screen, handed over.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The bureau is the Amiga screen as a shape of window (specs/bureau.md): one
 * full-screen window, always in backdrop mode -- the Workbench grey, flat, no
 * gadgets -- which is what every Amiga user turned on anyway. It is a trinket
 * client, the greeter's shape (specs/trinket.md): an Application owns the
 * console channel, a Window owns the console window, and a Panel filled with
 * the theme's BACKGROUND is the screen. It asks the console for the size -- a
 * full-screen window must match the mode the driver settled on, and a
 * hardcoded one is refused at create -- paints, says so, and halts. The
 * window persists because the console owns the slice; the process's memory
 * reclaims through the session's ordinary path, and what a bureau grows into
 * (icons, windows of one's own, the re-login that reaps this backdrop) is the
 * later arc's.
 *
 * It halts in on_started rather than returning from exec: exec's teardown
 * destroys the windows, and the backdrop must outlive the process.
 */

#include <aegir/bootstrap.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/panel.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/window.h>
#include <sel4/sel4.h>
#include <memory>

namespace {

void write(char const *text)
{
    aegir::debug_write(text);
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

    auto backdrop = std::make_unique<Panel>(Panel::Style::FLAT);
    backdrop->set_background(app.theme().color(ColorRole::BACKGROUND));
    window.set_content(std::move(backdrop));
    window.show();

    app.on_started = [&]() {
        write("  bureau: the screen is yours\n");
        if (log.valid()) {
            (void)log.call(aegir::log::kMethodEvent,
                           static_cast<uint64_t>(aegir::log::Event::Ready));
        }
        /* The exit is the handoff: the window stays (the console owns the
         * slice), the session's reclaim takes the rest, and auth goes back to
         * serving. */
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    };

    return app.exec();
}