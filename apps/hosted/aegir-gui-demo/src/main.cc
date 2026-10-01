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
 *
 * It is also the toolkit's widget test-bed (specs/trinket/layout.md): its
 * content is a framed Group -- a free terminal over a fixed label band -- and
 * the widgets land here as they arrive, so the runner sees them on the target
 * and not only in a host check.
 */

#include <aegir/bootstrap.h>
#include <aegir/bureau/menu.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/font.h>
#include <aegir/trinket/button.h>
#include <aegir/trinket/group.h>
#include <aegir/trinket/label.h>
#include <aegir/trinket/locale.h>
#include <aegir/trinket/panel.h>
#include <aegir/trinket/radio_group.h>
#include <aegir/trinket/scrollbar.h>
#include <aegir/trinket/slider.h>
#include <aegir/trinket/terminal_view.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/translation.h>
#include <aegir/trinket/window.h>
#include <sel4/sel4.h>
#include <cstdio>
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

    /* The face the label draws: a real TrueType face served by font.main, not
     * the embedded bitmap (specs/fonts.md). The Greek and Cyrillic words are
     * the visible part of the proof -- the Terminus the toolkit embeds has
     * neither -- and the cue below is the certain one: the client asks the
     * service for a glyph, gets a box back across its own transfer page, and
     * prints it. Declared before the window so it outlives the label that
     * points at it. */
    std::unique_ptr<Font> outline = app.load_service_font("Noto Sans", 16);
    if (outline != nullptr) {
        Glyph const *const a = outline->glyph(U'A');
        std::string line("  demo: outline A ");
        if (a != nullptr) {
            line += std::to_string(a->width);
            line += "x";
            line += std::to_string(a->height);
            line += " advance ";
            line += std::to_string(a->advance);
        } else {
            line += "missing";
        }
        line += "\n";
        write(line.c_str());
    } else {
        write("  demo: no font.main face for the outline label\n");
    }

    /* The band the outline line gets: the face's own line height, so the grid
     * keeps the geometry it had before the label and the acceptance's samples
     * of it stand (specs/fonts.md's phase 2 lands a widget beside the look, not
     * over it). Not a number chosen here -- it is what the face measures. The
     * content is a raised Group (specs/trinket/layout.md): its frame reaches two
     * pixels in on each side, so the window is that much taller and the grid
     * keeps its kWindowHeight. */
    Font* const label_font = outline != nullptr ? outline.get() : app.default_font();
    int const label_band = label_font != nullptr ? label_font->height() : 0;
    int const frame_inset = Group::frame_inset(Group::Frame::RAISED);
    /* The toggle row's band comes from what it holds (specs/trinket/checkbox.md,
     * specs/trinket/radio_group.md): the window grows by it, so the grid keeps
     * its kWindowHeight and only the label band moves down. A checkbox beside a
     * radio group -- three states, one of them exclusive. */
    int radio_changed = -1;
    /* The slider's value and whether the *user* moved it
     * (specs/trinket/slider.md); its cue is printed from on_poll. */
    int slider_value = 50;
    bool slider_moved = false;
    auto check = std::make_unique<Button>("Check", Button::Type::CHECK);
    check->set_checked(true);
    auto radios = std::make_unique<RadioGroup>(Group::Orientation::HORIZONTAL, 8);
    radios->set_frame(Group::Frame::GROUP_BOX);
    radios->set_title(U"Pick");
    radios->add("One");
    radios->add("Two");
    /* The cue is set after the members are added, so the group's first selection
     * does not read as a user's. */
    radios->on_changed = [&radio_changed](int index) { radio_changed = index; };
    auto toggles = std::make_unique<Group>(Group::Orientation::HORIZONTAL, 12);
    Button* const check_ptr = check.get();
    RadioGroup* const radios_ptr = radios.get();
    Group* const toggles_ptr = toggles.get();
    toggles->add_child(std::move(check));
    toggles->add_child(std::move(radios));
    toggles->set_weight(check_ptr, 0);
    toggles->set_weight(radios_ptr, 0);
    int const toggle_band = toggles->preferred_size().height;
    int const window_height =
        kWindowHeight + toggle_band + label_band + 2 * frame_inset;

    Window window(app);
    window.set_title("Demo");
    window.set_rect({kWindowX, kWindowY, kWindowWidth, window_height});
    window.set_gadgets(kGadgetClose | kGadgetZoom | kGadgetDepth);

    auto terminal = std::make_unique<TerminalView>();
    terminal->set_font(app.default_font());
    terminal->set_colors(app.theme().color(ColorRole::TEXT),
                         app.theme().color(ColorRole::WINDOW_BG));
    TerminalBuffer& grid = terminal->buffer();
    grid.write("Aegir terminal\n");
    grid.write("wide: \u65E5\u672C  combining: e\u0301\n");
    grid.write("rtl:  \u05E9\u05DC\u05D5\u05DD\n");
    for (int i = 1; i <= 100; ++i) {
        char line[24];
        std::snprintf(line, sizeof(line), "line %d\n", i);
        grid.write(line);
    }
    grid.scroll_to_bottom();

    /* The outline line sits in its own band at the window's foot, below the
     * grid. The content is a Group (specs/trinket/layout.md): a vertical group
     * whose terminal is free and whose label is fixed, so the grid takes the
     * slack and the band keeps the face's line height. */
    auto label = std::make_unique<Label>(
        U"Aegir fonts: a served .ttf face \u2014 \u0391\u03b2\u03b3 \u041f\u0440\u0438\u0432\u0435\u0442");
    if (outline != nullptr) {
        label->set_font(outline.get());
    }
    label->set_text_color(app.theme().color(ColorRole::TEXT));

    /* The terminal and its scrollbar in a row (specs/trinket/scrollbar.md):
     * the terminal free across the row, the scrollbar a fixed strip. A vertical
     * Group holds the row over the label band. The slider
     * (specs/trinket/slider.md) sits under the terminal, inside that same free
     * row, so the bands below keep the geometry the acceptance reads; its value
     * label beside it shows the number. */
    auto row = std::make_unique<Group>(Group::Orientation::HORIZONTAL, 0);
    TerminalView* const terminal_ptr = terminal.get();
    auto scrollbar = std::make_unique<Scrollbar>(Scrollbar::Orientation::VERTICAL);
    Scrollbar* const scrollbar_ptr = scrollbar.get();

    auto slider = std::make_unique<Slider>(Slider::Orientation::HORIZONTAL);
    slider->set_range(0, 100);
    slider->set_value(slider_value);
    slider->set_step(10);
    Slider* const slider_ptr = slider.get();
    auto slider_label = std::make_unique<Label>();
    Label* const slider_label_ptr = slider_label.get();
    slider_label->set_text(std::to_string(slider_value));
    slider_label->set_text_color(app.theme().color(ColorRole::TEXT));
    auto slider_row = std::make_unique<Group>(Group::Orientation::HORIZONTAL, 8);
    Group* const slider_row_ptr = slider_row.get();
    slider_row->add_child(std::move(slider));
    slider_row->add_child(std::move(slider_label));
    slider_row->set_weight(slider_label_ptr, 0);
    /* The value is the user's to change, so the cue is set from the widget's
     * own callback: a click, a drag and a key all end in on_change. */
    slider_ptr->on_change = [slider_label_ptr, &slider_value, &slider_moved](int v) {
        slider_value = v;
        slider_moved = true;
        slider_label_ptr->set_text(std::to_string(v));
    };

    auto terminal_column = std::make_unique<Group>(Group::Orientation::VERTICAL, 0);
    terminal_column->add_child(std::move(terminal));
    terminal_column->add_child(std::move(slider_row));
    terminal_column->set_weight(slider_row_ptr, 0);

    row->add_child(std::move(terminal_column));
    row->add_child(std::move(scrollbar));
    row->set_weight(scrollbar_ptr, 0);

    auto content = std::make_unique<Group>(Group::Orientation::VERTICAL, 0);
    content->set_frame(Group::Frame::RAISED);
    Label* const label_ptr = label.get();
    content->add_child(std::move(row));
    content->add_child(std::move(toggles));
    content->add_child(std::move(label));
    /* The label and the toggles keep their bands; the row is free and takes the
     * rest. */
    content->set_weight(label_ptr, 0);
    content->set_weight(toggles_ptr, 0);
    window.set_content(std::move(content));
    window.show();

    /* The scrollbar drives the terminal's scrollback (specs/trinket/scrollbar.md):
     * its value is the first visible line, and a scroll sets the buffer's offset
     * to match. It re-syncs each poll, so a resize that moved the grid is
     * reflected too. A *user* scroll sets a flag the poll prints, so a resize is
     * not mistaken for one. */
    bool scrolled = false;
    auto sync_scrollbar = [terminal_ptr, scrollbar_ptr]() {
        TerminalBuffer const& buffer = terminal_ptr->buffer();
        scrollbar_ptr->set_range(buffer.line_count(), buffer.rows());
        scrollbar_ptr->set_value(buffer.visible_first_line());
    };
    scrollbar_ptr->on_scroll = [terminal_ptr, &sync_scrollbar, &scrolled](int first) {
        TerminalBuffer& buffer = terminal_ptr->buffer();
        buffer.scroll_by(buffer.visible_first_line() - first);
        terminal_ptr->damage();
        sync_scrollbar();
        scrolled = true;
    };
    sync_scrollbar();

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
    int last_height = window_height;
    window.on_moved_resized = [app_ptr, &last_width, &last_height, window_height](Rect r) {
        if (r.width == last_width && r.height == last_height) return;
        last_width = r.width;
        last_height = r.height;
        int const screen_width = static_cast<int>(app_ptr->display_info().width_px);
        if (screen_width > 0 && r.width >= screen_width) {
            write("  demo: zoomed\n");
        } else if (r.width == kWindowWidth && r.height == window_height) {
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
        if (scrolled) {
            scrolled = false;
            write("  demo: scrolled\n");
        }
        if (radio_changed >= 0) {
            /* The radio group's selection is a cue (specs/trinket/radio_group.md):
             * choosing a member clears the others, and the runner reads which. */
            std::string line("  demo: radio ");
            line += std::to_string(radio_changed + 1);
            line += "\n";
            write(line.c_str());
            radio_changed = -1;
        }
        if (slider_moved) {
            /* The slider's value is a cue (specs/trinket/slider.md): the runner
             * reads the click, the drag and the key path back. */
            slider_moved = false;
            std::string line("  demo: slider ");
            line += std::to_string(slider_value);
            line += "\n";
            write(line.c_str());
        }
        sync_scrollbar();
        if (!registered) return;
        uint32_t const action = aegir::bureau::menu::take_action(bureau);
        if (action == 1) {
            write("  demo: about\n");
        } else if (action == 2) {
            write("  demo: reset\n");
        }
    };

    app.on_started = [&]() {
        write("  demo: terminal grid 3 scripts, 1 wide, 1 combining, 1 rtl\n");
        write("  demo: ready\n");
        if (log.valid()) {
            (void)log.call(aegir::log::kMethodEvent,
                           static_cast<uint64_t>(aegir::log::Event::Ready));
        }
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
    };

    return app.exec();
}
