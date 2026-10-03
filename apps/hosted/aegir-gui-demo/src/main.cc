/*
 * aegir-gui-demo: the window manager's demonstration client.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A decorated trinket window with all three titlebar gadgets -- close, zoom
 * and depth -- so the window manager's arc can be exercised end to end
 * (specs/window-manager.md). It is a command now, started in the session by
 * `Run gui-demo` in Sys:S/Shell-Startup, that creates its window and then
 * waits: the runner clicks the gadgets, and each act prints a cue. It asks for
 * no focus; a click on a gadget focuses it, and it does not want the shell's
 * keyboard.
 *
 * It is also the toolkit's widget test-bed (specs/trinket/layout.md): its
 * content is a framed Group -- a free terminal over a fixed label band -- and
 * the widgets land here as they arrive, so the runner sees them on the target
 * and not only in a host check.
 *
 * And it is the datatypes client's first caller (specs/datatypes.md): started
 * in the session, it holds the launch.session caller half, so it opens
 * Sys:TestImage.ilbm through the ilbm class and shows the frame the class
 * served on an Image tab. Running as a command is what gives it the caller's
 * authority; a boot service would have had the system's.
 */

#include <aegir/bootstrap.h>
#include <aegir/bureau/menu.h>
#include <aegir/console.h>
#include <aegir/datatypes_client.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/diagnostics.h>
#include <aegir/trinket/file_requester.h>
#include <aegir/trinket/font.h>
#include <aegir/trinket/button.h>
#include <aegir/trinket/cycle.h>
#include <aegir/trinket/group.h>
#include <aegir/trinket/label.h>
#include <aegir/trinket/listview.h>
#include <aegir/trinket/locale.h>
#include <aegir/trinket/panel.h>
#include <aegir/trinket/popup.h>
#include <aegir/trinket/popup_button.h>
#include <aegir/trinket/radio_group.h>
#include <aegir/trinket/scrollbar.h>
#include <aegir/trinket/slider.h>
#include <aegir/trinket/tab_group.h>
#include <aegir/trinket/terminal_view.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/translation.h>
#include <aegir/trinket/unicode.h>
#include <aegir/trinket/window.h>
#include <aegir/vfs.h>
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

/* The datatypes acceptance's view (specs/datatypes.md): the frame the ilbm
 * class served, converted to the RGBA the canvas blits. It is a Widget so the
 * layout places it and it reports the rectangle the acceptance can pin a pixel
 * in -- the cue the demo prints names its size, and the picture is the class's
 * output, not the toolkit's. */
class ImageView : public aegir::trinket::Widget {
public:
    ImageView(std::vector<uint8_t> rgba, int width, int height)
        : rgba_(std::move(rgba)), width_(width), height_(height) {}

    void on_paint(aegir::trinket::Canvas &canvas,
                  aegir::trinket::PaintEvent const &) override
    {
        /* Draw at the frame's own size, not the widget's: a group may hand a
         * child more room than it asked for, and the canvas's blit walks the
         * destination -- past the source if the two disagree. The extra room
         * stays the page's background. */
        aegir::trinket::Rect const r = rect();
        aegir::trinket::Rect const at{r.x, r.y, width_, height_};
        canvas.draw_bitmap(at, rgba_.data(), width_, height_, width_ * 4, true);
    }

    aegir::trinket::Size preferred_size() const override
    {
        return {width_, height_};
    }

    /* The frame's rectangle in screen coordinates, which the acceptance pins
     * its pixels in. Not `screen_rect_of`: that is for a part *within* a
     * widget, relative to its own rect, and it subtracts the widget's position.
     * The widget's own rectangle may be wider -- a group stretches a child --
     * but the picture is drawn width_ x height_ at the widget's origin. */
    aegir::trinket::Rect image_screen_rect() const
    {
        aegir::trinket::Rect const s = screen_rect();
        return {s.x, s.y, width_, height_};
    }

private:
    std::vector<uint8_t> rgba_;
    int width_;
    int height_;
};

/* Decode `path` through its datatype class (specs/datatypes.md), convert the
 * canonical frame to the canvas's RGBA, and wrap it in a view. `cue` names the
 * kind in the line the acceptance reads -- `demo: image 64x48 format 0 ...` --
 * so the class's stated layout, not only the window, is proved. Null when no
 * class claims the file or the read failed. */
std::unique_ptr<ImageView> load_image(aegir::trinket::Application &app, char const *path,
                                      char const *cue)
{
    aegir::datatypes::Decoded image;
    aegir::datatypes::Object object =
        aegir::datatypes::new_object(app.allocator(), app.scratch(), path);
    if (!object.valid()) {
        std::string line("  demo: no class for ");
        line += path;
        line += "\n";
        write(line.c_str());
        return nullptr;
    }

    std::unique_ptr<ImageView> view;
    if (!object.read(image)) {
        std::string line("  demo: read failed for ");
        line += path;
        line += "\n";
        write(line.c_str());
        object.dispose_object();
        return nullptr;
    }

    int const w = static_cast<int>(image.info.width);
    int const h = static_cast<int>(image.info.height);
    std::vector<uint8_t> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::size_t const row = static_cast<std::size_t>(y) * image.info.stride;
            uint8_t r = 0, g = 0, b = 0, a = 255;
            switch (image.info.format) {
            case aegir::datatypes::Format::INDEXED: {
                uint8_t const index = image.pixels[row + x];
                if (index < image.palette.size()) {
                    aegir::datatypes::Color const &c = image.palette[index];
                    r = c.r;
                    g = c.g;
                    b = c.b;
                }
                if (image.info.transparent && index == image.info.transparent_index) {
                    a = 0;
                }
                break;
            }
            case aegir::datatypes::Format::RGB:
                r = image.pixels[row + x * 3];
                g = image.pixels[row + x * 3 + 1];
                b = image.pixels[row + x * 3 + 2];
                break;
            case aegir::datatypes::Format::RGBA:
                r = image.pixels[row + x * 4];
                g = image.pixels[row + x * 4 + 1];
                b = image.pixels[row + x * 4 + 2];
                a = image.pixels[row + x * 4 + 3];
                break;
            case aegir::datatypes::Format::GREY:
                r = g = b = image.pixels[row + x];
                break;
            }
            std::size_t const p =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                 static_cast<std::size_t>(x)) *
                4;
            rgba[p] = r;
            rgba[p + 1] = g;
            rgba[p + 2] = b;
            rgba[p + 3] = a;
        }
    }

    std::string line("  demo: ");
    line += cue;
    line += " ";
    line += std::to_string(w);
    line += "x";
    line += std::to_string(h);
    line += " format ";
    line += std::to_string(static_cast<int>(image.info.format));
    line += " palette ";
    line += std::to_string(image.palette.size());
    line += "\n";
    write(line.c_str());
    view = std::make_unique<ImageView>(std::move(rgba), w, h);

    object.dispose_object();
    return view;
}

/* The demo's own place (specs/window-manager.md): set_rect is the client
 * area, and the frame's titlebar sits above it, so the window reads clear of
 * the test bed's red one and of the bureau's backdrop samples. It was dragged
 * here before login when it was a boot service; now it starts here, launched
 * by the session's Shell-Startup. */
constexpr int kWindowX = 900;
constexpr int kWindowY = 450;
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
        /* The file requester (specs/trinket/file_requester.md): the toolkit's
         * first VFS client, opened on a drawer of the system volume. */
        {3, U"Open...", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::NONE, {}},
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

    /* The datatypes client's first calls (specs/datatypes.md): open the two
     * fixtures, one through each class. The class is started under this
     * program's own badge by the session launcher -- the demo holds
     * launch.session as a command (specs/launch.md) -- and serves the frame a
     * page at a time; load_image converts it to the canvas's RGBA and prints
     * what the class stated. The two files are the same picture, so the two
     * pins reading the same colours is the cross-check between the classes. */
    std::unique_ptr<ImageView> image_view = load_image(app, "Sys:TestImage.ilbm", "image");
    std::unique_ptr<ImageView> png_view = load_image(app, "Sys:TestImage.png", "png");
    /* Content-first (specs/datatypes.md): a PNG named `.ilbm`, so the broker
     * must let the class, not the name, decide -- ilbm.datatype declines it and
     * png.datatype claims it by content. Decoded only to prove the walk (the
     * Image tab keeps its two); the cue is the RGBA the png class states, which
     * the ILBM class never produces. */
    static_cast<void>(load_image(app, "Sys:Mystery.ilbm", "mystery"));

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
    /* The cycle's chosen entry, when the *user* cycled it (specs/trinket/cycle.md);
     * its cue is printed from on_poll. */
    int cycle_index = -1;
    /* The list's chosen row and whether the *user* scrolled it
     * (specs/trinket/listview.md); both are printed from on_poll. */
    int list_selected = -1;
    bool list_scrolled = false;
    /* The entry the popup button's object was picked at (specs/trinket/popup.md). */
    int popup_pick = -1;
    /* Whether the window's popup is the popup button's object rather than the
     * cycle's menu. The window owns one popup layer and one cue callback, so the
     * client is what says which popup opened; the popup button sets this just
     * before it opens. The two cues must not read the same, because the
     * acceptance answers a cue with its steps and would fire the object's step
     * on the cycle's menu (scripts/targets.py). A popup is replaced only by the
     * opener's own call -- a pointer-down outside dismisses and is swallowed --
     * so the flag cannot go stale between an open and its close. */
    bool popup_is_object = false;
    /* The tab group's chosen page (specs/trinket/tabs.md): the runner's cue. */
    int tab_changed = -1;
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
    toggles->add_child(std::move(check));
    toggles->add_child(std::move(radios));
    toggles->set_weight(check_ptr, 0);
    toggles->set_weight(radios_ptr, 0);
    int const toggle_band = toggles->preferred_size().height;
    /* The horizontal bar's band, at the window's foot (specs/trinket/scrollbar.md):
     * the window grows by it, so the free terminal above gives up the height and
     * the rows the acceptance measures keep their y. */
    int const bar_band = app.theme().metric(MetricRole::SCROLLBAR_WIDTH);
    int const window_height =
        kWindowHeight + toggle_band + label_band + bar_band + 2 * frame_inset;

    Window window(app);
    window.set_title("Demo");
    window.set_rect({kWindowX, kWindowY, kWindowWidth, window_height});
    window.set_gadgets(kGadgetClose | kGadgetZoom | kGadgetDepth);

    auto terminal = std::make_unique<TerminalView>();
    /* A grid: a fixed advance, so the theme's monospace face. */
    terminal->set_font(app.theme().font_monospace());
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

    /* The widgets are built here and grouped into the tabs at the end
     * (specs/trinket/tabs.md): the terminal and its scrollbar
     * (specs/trinket/scrollbar.md), the slider (specs/trinket/slider.md), the
     * cycle and popup button (specs/trinket/cycle.md, popup_button.md) and the
     * list (specs/trinket/listview.md) are each a page's. */
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

    /* The cycle and the popup button (specs/trinket/cycle.md,
     * specs/trinket/popup_button.md) share a row above the slider's, so the
     * slider keeps the geometry the acceptance reads. */
    auto cycle = std::make_unique<Cycle>();
    Cycle* const cycle_ptr = cycle.get();
    /* The first entry is short and the one the acceptance cycles to is long, so
     * the ink in the text band tells the two apart (scripts/targets.py). */
    cycle->add(U"A500");
    cycle->add(U"Amiga 1200");
    cycle->add(U"A4000");
    cycle->on_changed = [&cycle_index](int index) { cycle_index = index; };
    auto popup = std::make_unique<PopupButton>(PopupButton::Role::POPUP);
    PopupButton* const popup_ptr = popup.get();
    /* The popup button opens an object of the client's: a small list, the way
     * the cycle opens its entries (specs/trinket/popup.md). Its cue is written
     * here rather than from on_poll so it precedes the menu's own, which the
     * window writes as the popup opens. */
    popup->on_click = [&window, &app, popup_ptr, &popup_pick, &popup_is_object]() {
        write("  demo: popup\n");
        /* The object is the list itself -- its own well is its border -- with
         * its entries centred, a menu like the cycle's (specs/trinket/popup.md). */
        auto object = std::make_unique<ListView>();
        for (std::u32string_view name : {U"Open", U"Save", U"Print"}) {
            object->add(name);
        }
        object->set_align(ListView::Alignment::CENTER);
        object->on_select = [&window, &popup_pick](int index) {
            window.close_popup();
            popup_pick = index;
        };
        int const width = app.theme().metric(MetricRole::LIST_MIN_WIDTH);
        Size const size{width, object->height_for_rows(3)};
        Rect const bounds{0, 0, window.rect().width, window.rect().height};
        /* Before the open: the window writes its cue from inside open_popup. */
        popup_is_object = true;
        window.open_popup(std::move(object), popup_rect(popup_ptr->rect(), size, bounds));
    };
    auto cycle_row = std::make_unique<Group>(Group::Orientation::HORIZONTAL, 8);
    Group* const cycle_row_ptr = cycle_row.get();
    cycle_row->add_child(std::move(cycle));
    cycle_row->add_child(std::move(popup));
    cycle_row->set_weight(popup_ptr, 0);

    /* The list and its scrollbar (specs/trinket/listview.md): rows with one
     * chosen, and the same Scrollbar the terminal uses as the control. It is
     * longer than the page shows, so its scrollbar has a range to travel -- the
     * acceptance scrolls it a row and reads the cue (scripts/targets.py). */
    auto list = std::make_unique<ListView>();
    for (std::u32string_view name :
         {U"C:", U"Fonts", U"Libs", U"Prefs", U"System", U"Tests", U"Tools",
          U"Users", U"Work", U"Games", U"Demos", U"Docs", U"Music", U"Images",
          U"Dev", U"Tmp", U"Ram", U"Disk", U"Archive", U"Backup"}) {
        list->add(name);
    }
    ListView* const list_ptr = list.get();
    list->set_active(0);  // chosen, but not by the user: no cue
    list->on_select = [&list_selected](int index) { list_selected = index; };
    auto list_scrollbar = std::make_unique<Scrollbar>(Scrollbar::Orientation::VERTICAL);
    Scrollbar* const list_scrollbar_ptr = list_scrollbar.get();
    auto list_row = std::make_unique<Group>(Group::Orientation::HORIZONTAL, 0);
    list_row->add_child(std::move(list));
    list_row->add_child(std::move(list_scrollbar));
    list_row->set_weight(list_scrollbar_ptr, 0);

    /* A horizontal bar at the window's foot (specs/trinket/scrollbar.md): the
     * test-bed's own control, drawing the MUI ArrowLeft/ArrowRight art. No view
     * here scrolls sideways -- that is the list/viewer arc's -- so it carries a
     * range of its own and the poll prints the value it lands on. It is the last
     * child of the content, so the free terminal above absorbs its band and every
     * row the acceptance measures keeps its y. */
    auto hbar = std::make_unique<Scrollbar>(Scrollbar::Orientation::HORIZONTAL);
    Scrollbar* const hbar_ptr = hbar.get();
    hbar_ptr->set_range(100, 25);
    hbar_ptr->set_value(40);
    bool bar_scrolled = false;
    int bar_value = 0;
    hbar_ptr->on_scroll = [&bar_scrolled, &bar_value](int v) {
        bar_value = v;
        bar_scrolled = true;
    };

    /* Group the widgets into tabs by kind (specs/trinket/tabs.md): Toggles,
     * Values, Lists and Text. A new widget is a page or a page's child, so the
     * test-bed grows by a tab rather than by another band. Lists opens first:
     * it holds the widgets the acceptance reads at boot. */
    auto tabs = std::make_unique<TabGroup>();
    TabGroup* const tabs_ptr = tabs.get();
    tabs->on_change = [&tab_changed](int index) { tab_changed = index; };

    /* Toggles: the checkbox and the radio group (specs/trinket/checkbox.md,
     * radio_group.md). */
    tabs->add_page(U"Toggles", std::move(toggles));

    /* Values: the cycle and popup button over the slider
     * (specs/trinket/cycle.md, popup_button.md, slider.md). */
    auto values_page = std::make_unique<Group>(Group::Orientation::VERTICAL, 8);
    values_page->add_child(std::move(cycle_row));
    values_page->add_child(std::move(slider_row));
    values_page->set_weight(cycle_row_ptr, 0);
    values_page->set_weight(slider_row_ptr, 0);
    tabs->add_page(U"Values", std::move(values_page));

    /* Lists: the list and its scrollbar over the horizontal bar
     * (specs/trinket/listview.md, scrollbar.md). */
    auto lists_page = std::make_unique<Group>(Group::Orientation::VERTICAL, 8);
    lists_page->add_child(std::move(list_row));
    lists_page->add_child(std::move(hbar));
    lists_page->set_weight(hbar_ptr, 0);
    tabs->add_page(U"Lists", std::move(lists_page));

    /* Text: the terminal and its scrollbar over the outline label
     * (specs/trinket/scrollbar.md, terminal.md). */
    auto text_row = std::make_unique<Group>(Group::Orientation::HORIZONTAL, 0);
    text_row->add_child(std::move(terminal));
    text_row->add_child(std::move(scrollbar));
    text_row->set_weight(scrollbar_ptr, 0);
    auto text_page = std::make_unique<Group>(Group::Orientation::VERTICAL, 0);
    Label* const label_ptr = label.get();
    text_page->add_child(std::move(text_row));
    text_page->add_child(std::move(label));
    text_page->set_weight(label_ptr, 0);
    tabs->add_page(U"Text", std::move(text_page));

    /* Image: the two decoded frames (specs/datatypes.md), the ILBM over the
     * PNG. Its own page, so the widgets the acceptance reads at boot keep their
     * geometry; the cues name each frame and the acceptance pins a pixel in
     * each reported rectangle. */
    auto image_page = std::make_unique<Group>(Group::Orientation::VERTICAL, 8);
    ImageView* const image_ptr = image_view.get();
    ImageView* const png_ptr = png_view.get();
    if (image_view != nullptr) {
        image_page->add_child(std::move(image_view));
    }
    if (png_view != nullptr) {
        image_page->add_child(std::move(png_view));
    }
    tabs->add_page(U"Image", std::move(image_page));

    tabs->set_active(2); /* Lists */
    auto content = std::make_unique<Group>(Group::Orientation::VERTICAL, 0);
    content->set_frame(Group::Frame::RAISED);
    content->add_child(std::move(tabs));
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

    /* The list's scrollbar is its control, as the terminal's is
     * (specs/trinket/listview.md): a user scroll sets the list's first visible
     * row, and it re-syncs each poll so a resize is reflected too. */
    auto sync_list = [list_ptr, list_scrollbar_ptr]() {
        list_scrollbar_ptr->set_range(list_ptr->count(), list_ptr->visible_rows());
        list_scrollbar_ptr->set_value(list_ptr->first());
    };
    list_scrollbar_ptr->on_scroll = [list_ptr, &sync_list, &list_scrolled](int first) {
        list_ptr->set_first(first);
        sync_list();
        list_scrolled = true;
    };
    sync_list();

    /* The demo's rect cues, in one place, because more than one path prints a
     * cue the next step answers: `on_moved_resized` prints "demo: zoomed" and
     * "demo: restored" from inside the geometry change, before `on_poll` would
     * report the new layout, so the report must come first or the runner clicks
     * the old rectangle (specs/testing.md: what a cue opens must already be
     * reported). The window lays its content out in `paint()` before it calls
     * `on_moved_resized`, so the content rectangles are current here. */
    auto report_demo = [&]() {
        window.report_rects("demo");
        tabs_ptr->report_parts("demo.tabs");
        report_rect("demo.list", *list_ptr);
        list_ptr->report_parts("demo.list");
        list_scrollbar_ptr->report_parts("demo.list_scrollbar");
        hbar_ptr->report_parts("demo.hbar");
        scrollbar_ptr->report_parts("demo.scrollbar");
        slider_ptr->report_parts("demo.slider");
        cycle_ptr->report_parts("demo.cycle");
        report_rect("demo.popup_button", *popup_ptr);
        radios_ptr->report_parts("demo.radios");
        report_rect("demo.check", *check_ptr);
        if (image_ptr != nullptr) {
            report_rect("demo.image", image_ptr->image_screen_rect());
            image_ptr->report_parts("demo.image");
        }
        if (png_ptr != nullptr) {
            report_rect("demo.png", png_ptr->image_screen_rect());
            png_ptr->report_parts("demo.png");
        }
    };

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
    window.on_moved_resized = [&](Rect r) {
        if (r.width == last_width && r.height == last_height) return;
        last_width = r.width;
        last_height = r.height;
        /* The cue the runner answers is this one, so the rectangles it clicks
         * come first (report_demo). */
        report_demo();
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
    /* The popup layer is the window's (specs/trinket/popup.md): a cue as one
     * opens and closes, so the runner can pace a dump on it. The cycle's menu
     * and the popup button's object are both this window's. */
    window.on_popup = [&window, &popup_is_object](bool up) {
        /* The popup's own rectangle first, so the runner has it in hand before
         * the cue line that makes it click a row (specs/testing.md's rect cues;
         * a cue opens its step, so what the step clicks must already be
         * reported). */
        if (up && window.popup() != nullptr) {
            report_rect("demo.popup", *window.popup());
            window.popup()->report_parts("demo.popup");
        }
        if (popup_is_object) {
            write(up ? "  demo: object 1\n" : "  demo: object 0\n");
        } else {
            write(up ? "  demo: menu 1\n" : "  demo: menu 0\n");
        }
        /* Cleared at the close, so the next popup's opener decides the kind
         * afresh (the cycle's menu opener does not set it). */
        if (!up) popup_is_object = false;
    };

    /* The file requester (specs/trinket/file_requester.md): built before exec so
     * its window is reserved -- the console sizes one slice at exec -- and shown
     * by the Demo menu's Open... item. The VFS is this program's own, through
     * the manifest's vfs.namespace grant; the drawer it opens on is the system
     * volume, whose entries the runner reads back. */
    aegir::vfs::Namespace vfs = aegir::vfs::Namespace::find();
    auto file_requester = std::make_unique<FileRequester>(app, vfs, U"Open File");
    file_requester->open_at(U"Sys:");
    /* The requester's cues (specs/trinket/file_requester.md): the acceptance
     * paces a dump on each, so the pattern's filter and the Volumes list are
     * read back from a screendump. The pattern cue carries the wildcard and the
     * rows it kept. */
    file_requester->on_filter = [&file_requester](std::u32string const& pattern, int count) {
        /* The requester's rectangles first: this cue is printed from inside a
         * dispatch, before on_poll would report them, and the step it opens
         * clicks the Volumes button (specs/testing.md). */
        file_requester->report_parts("demo.requester");
        std::string line("  demo: filtered ");
        line += utf32_to_utf8(pattern);
        line += " ";
        line += std::to_string(count);
        line += "\n";
        write(line.c_str());
    };
    file_requester->on_action = [&file_requester](FileRequester::Action action) {
        file_requester->report_parts("demo.requester");
        if (action == FileRequester::OK) {
            write("  demo: opened ");
            write(utf32_to_utf8(file_requester->chosen()).c_str());
            write("\n");
        } else if (action == FileRequester::VOLUMES) {
            /* The browsable volumes, counted: NIL: and PIPE: carry kFlagNoDir
             * and are not among them (specs/trinket/file_requester.md). */
            std::string line("  demo: volumes ");
            line += std::to_string(file_requester->row_count());
            line += "\n";
            write(line.c_str());
        }
    };

    /* The bureau rings the doorbell for an action; fetch it and print the cue
     * the runner reads. Nothing to fetch until the tree is registered. */
    bool reported_rects = false;
    app.on_poll = [&]() {
        /* The rect cues (specs/testing.md): report where the widgets stand, so
         * the acceptance clicks them by name and a font or metric change moves
         * the click with the widget. Emitted before the cue lines below, because
         * the runner answers a cue by sending the next step's events -- each
         * rectangle must be in hand before the cue that clicks it. A widget on a
         * hidden page keeps the rectangle it had when last shown (the layouts
         * skip what is not visible), so a cue is never a collapsed rectangle. */
        report_demo();
        if (!reported_rects) {
            /* The demo is a session command now, so it may be up only after
             * login and the acceptance cannot rely on an earlier poll having
             * reported its rectangles. This cue comes after the report, and
             * the first demo gesture triggers on it (scripts/targets.py). */
            reported_rects = true;
            write("  demo: rects\n");
        }
        if (file_requester->visible()) {
            file_requester->report_parts("demo.requester");
        }
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
        if (tab_changed >= 0) {
            /* The tab group's chosen page (specs/trinket/tabs.md): the runner
             * reads which tab a click or a key landed on. */
            std::string line("  demo: tab ");
            line += std::to_string(tab_changed + 1);
            line += "\n";
            write(line.c_str());
            tab_changed = -1;
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
        if (cycle_index >= 0) {
            /* The cycle's active entry is a cue (specs/trinket/cycle.md): the
             * runner reads which one it landed on. */
            std::string line("  demo: cycle ");
            line += std::to_string(cycle_index + 1);
            line += "\n";
            write(line.c_str());
            cycle_index = -1;
        }
        if (popup_pick >= 0) {
            /* The popup button's object was picked (specs/trinket/popup.md). */
            std::string line("  demo: picked ");
            line += std::to_string(popup_pick + 1);
            line += "\n";
            write(line.c_str());
            popup_pick = -1;
        }
        if (list_selected >= 0) {
            /* The list's chosen row is a cue (specs/trinket/listview.md): the
             * runner reads which one it landed on. */
            std::string line("  demo: list ");
            line += std::to_string(list_selected + 1);
            line += "\n";
            write(line.c_str());
            list_selected = -1;
        }
        if (list_scrolled) {
            /* The user scrolled the list: its first visible row is a cue, as the
             * terminal's scroll is (specs/trinket/scrollbar.md). */
            list_scrolled = false;
            std::string line("  demo: listed ");
            line += std::to_string(list_ptr->first());
            line += "\n";
            write(line.c_str());
        }
        if (bar_scrolled) {
            /* The horizontal bar's own control moved (specs/trinket/scrollbar.md):
             * the value it landed on is the cue, as the terminal's scroll and the
             * list's are. */
            bar_scrolled = false;
            std::string line("  demo: bar ");
            line += std::to_string(bar_value);
            line += "\n";
            write(line.c_str());
        }
        sync_scrollbar();
        sync_list();
        if (!registered) return;
        uint32_t const action = aegir::bureau::menu::take_action(bureau);
        if (action == 1) {
            write("  demo: about\n");
        } else if (action == 2) {
            write("  demo: reset\n");
        } else if (action == 3) {
            /* Open...: the file requester, on the drawer it was built with. The
             * cue paces a dump on it (specs/trinket/file_requester.md), and it
             * is written after the show so the dump sees the window rather than
             * the frame before it. */
            file_requester->show();
            /* Its rectangles before the cue: the next step types into the
             * Pattern box, and the step after that clicks a button
             * (specs/testing.md). */
            file_requester->report_parts("demo.requester");
            write("  demo: requester up\n");
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
