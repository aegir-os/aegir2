/*
 * aegir-editor: the toolkit's multiline editor application.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A launcher starts this as a windowed program (specs/launch.md's kind 2): its
 * own console.gui, its own runtime, its own window. The window's content is a
 * TabGroup (specs/trinket/tabs.md) with one TextEdit page per open file, so a
 * file is a tab and the tab strip is what says which one is up. The File menu
 * opens and saves through the session's namespace, and the cues it prints are
 * what the acceptance reads the widget back by (specs/trinket/editor.md).
 */

#include <aegir/bootstrap.h>
#include <aegir/bureau/menu.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/environment.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/file_path.h>
#include <aegir/trinket/file_requester.h>
#include <aegir/trinket/group.h>
#include <aegir/trinket/menubar.h>
#include <aegir/trinket/scrollbar.h>
#include <aegir/trinket/tab_group.h>
#include <aegir/trinket/text_edit.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/unicode.h>
#include <aegir/trinket/window.h>
#include <aegir/trinket/window_spec.h>
#include <aegir/vfs.h>
#include <sel4/sel4.h>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void write(char const *text)
{
    aegir::debug_write(text);
}

void write_path(char const *label, std::string const &path)
{
    write(label);
    write(path.c_str());
    write("\n");
}

/* Clear of the terminal's window and the demo's, so a screendump's samples
 * never share. A launcher's AEGIR_WINDOW overrides it (specs/launch.md). */
constexpr int kWindowX = 420;
constexpr int kWindowY = 200;
constexpr int kWindowWidth = 520;
constexpr int kWindowHeight = 340;

/* A file's bytes through the session's namespace (vfs.namespace, which the
 * launcher granted), or an empty string when it cannot be read. */
std::string read_file(char const *path)
{
    int const fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return {};
    }
    std::string text;
    char chunk[512];
    ssize_t have = 0;
    while ((have = ::read(fd, chunk, sizeof(chunk))) > 0) {
        text.append(chunk, static_cast<std::size_t>(have));
    }
    ::close(fd);
    return text;
}

/* Replace `path` with `bytes`: the write side of the namespace (specs/vfs.md),
 * through the runtime's POSIX layer (specs/cxx.md step 5). False when the
 * volume refused. */
bool write_file(char const *path, std::string const &bytes)
{
    int const fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        ssize_t const wrote = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (wrote <= 0) {
            ::close(fd);
            return false;
        }
        done += static_cast<std::size_t>(wrote);
    }
    ::close(fd);
    return true;
}

/* The name a tab shows: the path's last component, past a drawer's slash or a
 * volume's colon. An empty leaf -- a drawer itself -- has no name of its own. */
std::u32string tab_name(std::string const &path)
{
    std::size_t start = 0;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' || path[i] == ':') {
            start = i + 1;
        }
    }
    std::string const leaf = path.substr(start);
    return leaf.empty() ? std::u32string(U"Untitled")
                        : aegir::trinket::utf8_to_utf32(leaf);
}

/* The editor's menus, registered with the bureau's menu server
 * (specs/workbench.md): the screen bar carries them while the editor is the
 * active window. */
std::vector<aegir::trinket::MenuBar::Menu> editor_menus()
{
    using MenuItem = aegir::trinket::MenuBar::MenuItem;
    using Menu = aegir::trinket::MenuBar::Menu;
    Menu file;
    file.title = U"Ed";
    file.items = {
        {1, U"New", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::NONE, {}},
        /* Open... and Save As... raise the toolkit's requester
         * (specs/trinket/file_requester.md). */
        {2, U"Open...", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::NONE, {}},
        {3, U"Save", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::NONE, {}},
        {4, U"Save As...", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::NONE, {}},
        {5, U"Quit", aegir::trinket::KeyCode::UNKNOWN, 0, MenuItem::NONE, {}},
    };
    return {std::move(file)};
}

} // namespace

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
        write("  editor: FAIL no console.gui\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    app.set_gui_port(gui);

    /* The bureau.menu port (specs/workbench.md): the editor registers its tree
     * when it first gains the focus, reports focus as it changes, and fetches
     * the action the bureau rings its doorbell for. */
    aegir::ipc::Consumer const bureau = aegir::ipc::Consumer::find(
        aegir::bureau::menu::kPortName, aegir::bureau::menu::kPortNameLength);

    Window window(app);
    window.set_title("Ed");
    window.set_rect({kWindowX, kWindowY, kWindowWidth, kWindowHeight});
    /* Close and zoom gadgets (specs/window-manager.md), with the depth gadget a
     * decorated window carries by default. Closing the editor exits it. */
    window.set_gadgets(kGadgetClose | kGadgetZoom | kGadgetDepth);
    /* A launcher sets AEGIR_WINDOW (specs/launch.md): the specification is this
     * program's window, and the program that owns the window parses it. */
    char const *const spec = aegir::environment::getenv("AEGIR_WINDOW");
    if (spec != nullptr) {
        std::string title;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        if (parse_window_spec(spec, &x, &y, &width, &height, &title)) {
            window.set_rect({x, y, width, height});
            if (!title.empty()) {
                window.set_title(title.c_str());
            }
        }
    }

    /* The window is the tabs (specs/trinket/tabs.md): one page per open file. */
    auto tabs = std::make_unique<TabGroup>();
    TabGroup *const tabs_ptr = tabs.get();

    /* What the editor knows about a page: the file it came from, and the two
     * widgets the TabGroup owns -- the view and the scrollbar it drives. */
    struct Tab {
        std::string path;
        TextEdit *edit = nullptr;
        Scrollbar *bar = nullptr;
        std::function<void()> sync;
    };
    std::vector<Tab> open;

    /* A page: the editor free across the row and its scrollbar a fixed strip,
     * the shape the demo's terminal page has (specs/trinket/scrollbar.md). The
     * view drives the scrollbar and the scrollbar the view. */
    auto add_tab = [&](std::string const &path, std::string const &text) {
        auto page = std::make_unique<Group>(Group::Orientation::HORIZONTAL, 0);
        auto edit = std::make_unique<TextEdit>();
        TextEdit *const view = edit.get();
        auto bar = std::make_unique<Scrollbar>(Scrollbar::Orientation::VERTICAL);
        Scrollbar *const scroll = bar.get();
        view->set_font(app.default_font());
        view->set_colors(app.theme().color(ColorRole::TEXT),
                         app.theme().color(ColorRole::WINDOW_BG));
        if (!text.empty()) {
            view->set_text(text);
        }
        page->add_child(std::move(edit));
        page->add_child(std::move(bar));
        page->set_weight(scroll, 0);

        Tab tab;
        tab.path = path;
        tab.edit = view;
        tab.bar = scroll;
        tab.sync = [view, scroll]() {
            scroll->set_range(view->row_count(), view->visible_rows());
            scroll->set_value(view->first_row());
        };
        view->on_change = tab.sync;
        scroll->on_scroll = [view](int first) { view->set_first_row(first); };
        open.push_back(std::move(tab));

        tabs_ptr->add_page(path.empty() ? std::u32string(U"Untitled") : tab_name(path),
                           std::move(page));
        tabs_ptr->set_active(tabs_ptr->page_count() - 1);
        window.set_focus(view);
        open.back().sync();
    };

    /* The requester (specs/trinket/file_requester.md): the toolkit's own, over
     * the session's namespace. One serves Open... and Save As...; its window is
     * built before exec, because the console sizes one slice at exec. */
    aegir::vfs::Namespace vfs = aegir::vfs::Namespace::find();
    auto requester = std::make_unique<FileRequester>(app, vfs, U"Open File");
    requester->open_at(U"Sys:");
    bool saving = false;

    /* The File menu's actions, from the bureau's doorbell (specs/workbench.md). */
    auto save_active = [&]() {
        if (open.empty()) {
            return;
        }
        Tab &tab = open[static_cast<std::size_t>(tabs_ptr->active())];
        if (tab.path.empty()) {
            return; /* no name yet: Save As... has to name it */
        }
        if (write_file(tab.path.c_str(), utf32_to_utf8(tab.edit->text()))) {
            write_path("  editor: saved ", tab.path);
        } else {
            write_path("  editor: cannot save ", tab.path);
        }
    };

    requester->on_action = [&](FileRequester::Action action) {
        if (action != FileRequester::OK) {
            return;
        }
        std::u32string const full =
            file_path::join(requester->drawer(), requester->chosen());
        std::string const path = utf32_to_utf8(full);
        if (saving) {
            saving = false;
            if (open.empty()) {
                return;
            }
            Tab &tab = open[static_cast<std::size_t>(tabs_ptr->active())];
            if (write_file(path.c_str(), utf32_to_utf8(tab.edit->text()))) {
                tab.path = path;
                tabs_ptr->set_title(tabs_ptr->active(), tab_name(path));
                /* Save As... saves under a new name; Save overwrites the one a
                 * tab already has, and the two cues read apart (a step answers
                 * one cue, specs/trinket/editor.md). */
                write_path("  editor: saved as ", path);
            } else {
                write_path("  editor: cannot save ", path);
            }
            return;
        }
        std::string const text = read_file(path.c_str());
        if (text.empty()) {
            write_path("  editor: cannot read ", path);
            return;
        }
        add_tab(path, text);
        write_path("  editor: opened ", path);
    };

    /* The bureau.menu registration (specs/workbench.md): a menu tree crosses
     * once the window has the focus, and the doorbell wakes the poll that
     * fetches an action. A *session* client (this one) starts after the bureau,
     * so its first focus is enough to know the port has an owner; the console's
     * screen-owner nudge -- which a client started at boot waits for, because
     * calling a port with no owner blocks -- may already have passed it by. */
    bool bureau_up = false;
    bool focus_seen = false;
    bool registered = false;
    bool want_active = false;
    auto ensure_registered = [&]() {
        if (registered || !bureau.valid() || (!bureau_up && !focus_seen)) {
            return;
        }
        seL4_CPtr const doorbell = app.alloc_slot();
        if (doorbell != 0 && app.mint_event_notification(doorbell) &&
            aegir::bureau::menu::register_menus(bureau, editor_menus(), doorbell)) {
            registered = true;
            write("  editor: menus up\n");
        }
    };
    auto report_focus = [&](bool is_active) {
        want_active = is_active;
        if (is_active) {
            focus_seen = true;
        }
        ensure_registered();
        if (registered) {
            (void)aegir::bureau::menu::set_active(bureau, is_active);
        }
        /* The window's focus is the screen bar's: dropping the menu needs the
         * bureau to know whose it is, and the requester (a window of its own,
         * focused and then gone) leaves no window focused when it closes, so
         * the editor has to be clicked back. These cues pace that -- the
         * set_active has returned by the time one prints. */
        write(is_active ? "  editor: active\n" : "  editor: away\n");
    };
    app.on_screen_owner = [&](bool up) {
        if (!up) {
            return;
        }
        bureau_up = true;
        ensure_registered();
        if (registered) {
            (void)aegir::bureau::menu::set_active(bureau, want_active);
        }
    };
    window.on_focus_changed = [&](bool is_active) { report_focus(is_active); };

    /* The tab the user picked becomes the one the keys reach, and prints its
     * cue; the strip's change is the TabGroup's own (specs/trinket/tabs.md). */
    tabs_ptr->on_change = [&](int index) {
        if (index < 0 || index >= static_cast<int>(open.size())) {
            return;
        }
        window.set_focus(open[static_cast<std::size_t>(index)].edit);
        write("  editor: tab ");
        write(std::to_string(index + 1).c_str());
        write("\n");
    };

    if (argc > 1 && argv[1] != nullptr) {
        std::string const text = read_file(argv[1]);
        if (text.empty()) {
            write_path("  editor: cannot read ", argv[1]);
            add_tab("", "");
        } else {
            add_tab(argv[1], text);
            write_path("  editor: opened ", argv[1]);
        }
    } else {
        add_tab("", "");
    }

    window.set_content(std::move(tabs));
    window.show();
    window.request_focus();

    /* The insert/overwrite cue (specs/trinket/editor.md): the mode is the
     * document's, and the cursor's shape follows it, so the poll reports the
     * change the keys made. */
    TextDocument::Mode last_mode = TextDocument::Mode::INSERT;

    app.on_poll = [&]() {
        if (!open.empty()) {
            Tab const &tab = open[static_cast<std::size_t>(tabs_ptr->active())];
            tab.sync();
            if (tab.edit->document().mode() != last_mode) {
                last_mode = tab.edit->document().mode();
                write(last_mode == TextDocument::Mode::INSERT ? "  editor: insert\n"
                                                              : "  editor: overwrite\n");
            }
        }
        if (!registered) {
            return;
        }
        uint32_t const action = aegir::bureau::menu::take_action(bureau);
        switch (action) {
        case 1: /* New */
            add_tab("", "");
            write("  editor: new\n");
            break;
        case 2: /* Open... */
            saving = false;
            /* Open sorts the system volume, where the readable files are; Save
             * As sorts the session's own Home:, which Sys: -- read-only -- is
             * not, so a save has somewhere to go (specs/auth.md). */
            requester->open_at(U"Sys:");
            requester->show();
            write("  editor: open requester\n");
            break;
        case 3: /* Save */
            save_active();
            break;
        case 4: /* Save As... */
            saving = true;
            requester->open_at(U"Home:");
            requester->show();
            write("  editor: save requester\n");
            break;
        case 5: /* Quit */
            /* Tell the bureau the editor is no longer the active client before
             * it goes: the registry would otherwise keep its tree, and the
             * screen bar would never drop the bureau's own menus again -- the
             * exit is not a window the console can report focus for. */
            report_focus(false);
            write("  editor: quit\n");
            app.quit(0);
            break;
        default:
            break;
        }
    };

    window.on_close_requested = [&]() {
        report_focus(false);
        write("  editor: closed\n");
        app.quit(0);
    };

    app.on_started = [&]() {
        write("  editor: ready\n");
        if (log.valid()) {
            (void)log.call(aegir::log::kMethodEvent,
                           static_cast<uint64_t>(aegir::log::Event::Ready));
        }
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
    };

    return app.exec();
}
