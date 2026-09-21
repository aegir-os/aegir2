/*
 * aegir-gui-demo: the window manager's demonstration client.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A decorated trinket window with all three titlebar gadgets -- close, zoom
 * and depth -- so the window manager's arc can be exercised end to end
 * (specs/window-manager.md). It is a boot service, spawned by director, that
 * creates its window and then waits: the runner clicks the gadgets, and each
 * act prints a cue. It asks for no focus; a click on a gadget focuses it, and
 * it does not want the boot's keyboard.
 */

#include <aegir/bootstrap.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/label.h>
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

/* Clear of the greeter's window and of the test bed's, so its pixels and
 * theirs never share a sample. */
constexpr int kWindowX = 900;
constexpr int kWindowY = 300;
constexpr int kWindowWidth = 260;
constexpr int kWindowHeight = 200;

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
        write("  demo: FAIL no console.gui\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    app.set_gui_port(gui);

    Window window(app);
    window.set_title("Demo");
    window.set_rect({kWindowX, kWindowY, kWindowWidth, kWindowHeight});
    window.set_gadgets(true, true, true);

    auto panel = std::make_unique<Panel>(Panel::Style::FLAT);
    panel->set_background(app.theme().color(ColorRole::WINDOW_BG));
    auto label = std::make_unique<Label>("the window manager");
    label->set_text_color(app.theme().color(ColorRole::TEXT));
    label->set_rect({16, 16, 220, 12});
    panel->add_child(std::move(label));
    window.set_content(std::move(panel));
    window.show();

    /* Each act prints its cue: the geometry a zoom or resize leaves, and the
     * close. The runner paces its dumps on them (scripts/targets.py). */
    window.on_moved_resized = [](Rect r) {
        write("  demo: geometry ");
        aegir::debug_write_unsigned(static_cast<uint64_t>(r.width));
        write("x");
        aegir::debug_write_unsigned(static_cast<uint64_t>(r.height));
        write("\n");
    };
    window.on_close_requested = []() { write("  demo: closed\n"); };

    app.on_started = [&]() {
        write("  demo: ready\n");
        if (log.valid()) {
            (void)log.call(aegir::log::kMethodEvent,
                           static_cast<uint64_t>(aegir::log::Event::Ready));
        }
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
    };

    return app.exec();
}
