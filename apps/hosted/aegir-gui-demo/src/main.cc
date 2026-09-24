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
#include <aegir/bureau/menu.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/label.h>
#include <aegir/trinket/locale.h>
#include <aegir/trinket/panel.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/translation.h>
#include <aegir/trinket/window.h>
#include <sel4/sel4.h>
#include <memory>
#include <string>
#include <vector>

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

/* The demo's menus, the first a client registers with the bureau
 * (specs/workbench.md): shown in the screen bar while the demo's window is
 * active, in place of the bureau's own. */
std::vector<aegir::trinket::MenuBar::Menu> demo_menus()
{
    using MenuItem = aegir::trinket::MenuBar::MenuItem;
    using Menu = aegir::trinket::MenuBar::Menu;
    Menu demo;
    demo.title = U"Demo";
    demo.items = {
        {1, U"About Demo", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::NONE, {}},
        {2, U"Reset", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::NONE, {}},
    };
    return {std::move(demo)};
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

    /* The toolkit's Locale on this target (specs/locale.md): the embedded
     * .locale blob is parsed here and formats a currency and a date, so the
     * cue proves the compiled CLDR data, not just the host-side conformance.
     * German exercises the swapped separators and the suffix currency. */
    {
        Locale const locale("de");
        write("  demo: locale ");
        write(locale.name().c_str());
        write(" says ");
        write(locale.format_currency(1234.5, "EUR").c_str());
        write(" on ");
        write(locale.format_date(0).c_str());
        write("\n");
    }

    /* The toolkit's gettext catalogue on this target (specs/locale.md): the
     * embedded .mo is parsed here and a string and a plural are translated
     * through it, so the cue proves the parser and the compiled catalogue. */
    {
        std::unique_ptr<Translation> const translation = Translation::embedded();
        if (translation != nullptr) {
            write("  demo: translation says ");
            write(translation->translate("Hello, world").c_str());
            write(" and ");
            write(translation->ntranslate("%d file", "%d files", 3).c_str());
            write("\n");
        }
    }

    aegir::ipc::Consumer const gui = aegir::ipc::Consumer::find(
        aegir::console::kPortName, aegir::console::kPortNameLength);
    if (!gui.valid()) {
        write("  demo: FAIL no console.gui\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    app.set_gui_port(gui);

    /* The bureau.menu port (specs/workbench.md): the demo is its first client.
     * It registers its tree when it first gains the focus, reports focus as it
     * changes, and fetches the action the bureau rings its doorbell for. */
    aegir::ipc::Consumer const bureau = aegir::ipc::Consumer::find(
        aegir::bureau::menu::kPortName, aegir::bureau::menu::kPortNameLength);

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
    /* Only the two landmarks print: a debug write is a syscall a character,
     * and logging every resize motion would tax the very gesture it reports.
     * An interactive resize lands between them and stays quiet. */
    Application *const app_ptr = &app;
    /* The cue is a change of *size*, not of place: a move carries the same
     * size, and a plain move must not read as a restore -- the demo is moved
     * before the bureau exists (specs/workbench.md). */
    int last_width = kWindowWidth;
    int last_height = kWindowHeight;
    window.on_moved_resized = [app_ptr, &last_width, &last_height](Rect r) {
        if (r.width == last_width && r.height == last_height) return;
        last_width = r.width;
        last_height = r.height;
        int const screen_width = static_cast<int>(app_ptr->display_info().width_px);
        if (screen_width > 0 && r.width >= screen_width) {
            write("  demo: zoomed\n");
        } else if (r.width == kWindowWidth && r.height == kWindowHeight) {
            write("  demo: restored\n");
        }
    };
    /* Register with the bureau, and report focus as it changes: the bureau
     * needs to know whose window's menus stand (specs/workbench.md). The
     * bureau is a session that starts at login, so the demo cannot call it
     * before then -- a call to a port with no owner blocks, which is what
     * froze the window when it was focused before login. The console's
     * screen-owner nudge is the fix: it tells every client when the bureau's
     * backdrop appears, and the demo registers then. Until the nudge it only
     * remembers the focus. The doorbell is a signal-only copy of the
     * notification the demo already listens on, so the bureau can wake it
     * when an item is clicked. */
    bool bureau_up = false;
    bool registered = false;
    bool want_active = false;
    auto ensure_registered = [&]() {
        if (registered || !bureau_up || !bureau.valid()) return;
        seL4_CPtr const doorbell = app.alloc_slot();
        if (doorbell != 0 && app.mint_event_notification(doorbell) &&
            aegir::bureau::menu::register_menus(bureau, demo_menus(), doorbell)) {
            registered = true;
            write("  demo: menus up\n");
        }
    };
    auto report_focus = [&](bool active) {
        want_active = active;
        ensure_registered();
        if (registered) {
            (void)aegir::bureau::menu::set_active(bureau, active);
        }
    };
    app.on_screen_owner = [&](bool up) {
        if (!up) return;
        bureau_up = true;
        ensure_registered();
        if (registered) {
            (void)aegir::bureau::menu::set_active(bureau, want_active);
        }
    };
    window.on_focus_changed = [&](bool active) { report_focus(active); };
    window.on_close_requested = [&]() {
        report_focus(false);
        write("  demo: closed\n");
    };

    /* The bureau rings the doorbell for an action; fetch it and print the cue
     * the runner reads. Nothing to fetch until the tree is registered. */
    app.on_poll = [&]() {
        if (!registered) return;
        uint32_t const action = aegir::bureau::menu::take_action(bureau);
        if (action == 1) {
            write("  demo: about\n");
        } else if (action == 2) {
            write("  demo: reset\n");
        }
    };

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
