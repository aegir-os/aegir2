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
#include <aegir/bureau/menu.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/launch_client.h>
#include <aegir/log.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/requester.h>
#include <aegir/trinket/textbox.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/unicode.h>
#include <aegir/trinket/window.h>
#include <sel4/sel4.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

void write(char const *text)
{
    aegir::debug_write(text);
}

/* Split a command line into the wire's one argv string (specs/launch.md): the
 * words NUL-separated, program first. Whitespace separates and a double-quoted
 * run is one word, so a name with spaces rides. No substitution and no
 * redirection -- this is the Bureau's Execute, a command line, not a Shell
 * line; a program that wants a Shell launches one. */
std::string split_argv(std::string_view line)
{
    std::string out;
    bool first = true;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        std::string word;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            if (line[i] == '"') {
                ++i;
                while (i < line.size() && line[i] != '"') {
                    word.push_back(line[i++]);
                }
                if (i < line.size()) {
                    ++i; /* the closing quote */
                }
            } else {
                word.push_back(line[i++]);
            }
        }
        if (!first) {
            out.push_back('\0');
        }
        out += word;
        first = false;
    }
    return out;
}

/* Execute a typed line (specs/launch.md): split it to argv and ask the session's
 * launcher for the command, carrying this process's own context. The Bureau has
 * no console stream, so the launcher gives the command its read-only output
 * view. */
void execute_command(std::string const &line)
{
    std::string const argv = split_argv(line);
    if (argv.empty()) {
        return;
    }
    std::string cue("  bureau: execute ");
    cue += line;
    cue.push_back('\n');
    write(cue.c_str());
    (void)aegir::launch::command(argv.data(), static_cast<uint32_t>(argv.size()), "", 0, "", 0,
                                 false);
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
        {8, U"Execute...", aegir::trinket::KeyCode::SPACE, aegir::trinket::kModSuper,
         MenuItem::NONE, {}},
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

/* The bureau.menu registry (specs/workbench.md): the clients that have
 * registered, their trees, the doorbell each is rung on, and which one is
 * active. A client is its badge -- the kernel's word for who called, not
 * anything it says -- and one badge has one entry. */
struct Client {
    seL4_Word badge = 0;
    std::vector<aegir::bureau::Desktop::Menu> menus;
    seL4_CPtr doorbell = 0;
    uint32_t pending = 0;
    bool active = false;
};

class Registry {
public:
    Client& ensure(seL4_Word badge)
    {
        for (Client& client : clients_) {
            if (client.badge == badge) return client;
        }
        clients_.push_back(Client{});
        clients_.back().badge = badge;
        return clients_.back();
    }

    Client* find(seL4_Word badge)
    {
        for (Client& client : clients_) {
            if (client.badge == badge) return &client;
        }
        return nullptr;
    }

    /* Only one client is active: gaining focus clears the rest. Losing it
     * clears only the one, so a focus-lost that races a focus-gained does not
     * take the new client's menus down. */
    void set_active(seL4_Word badge, bool on)
    {
        for (Client& client : clients_) {
            if (on) {
                client.active = client.badge == badge;
            } else if (client.badge == badge) {
                client.active = false;
            }
        }
    }

    Client* active()
    {
        for (Client& client : clients_) {
            if (client.active) return &client;
        }
        return nullptr;
    }

private:
    std::vector<Client> clients_;
};

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
    aegir::bureau::Desktop* const desktop_ptr = desktop.get();
    Registry registry;

    /* The Bureau's Execute (specs/launch.md): a requester with a command line,
     * built and reserved before the loop, so the console sizes the slice for its
     * window. OK, Return or the OK button runs the line through the session's
     * launcher, sending this process's own context. */
    auto execute_box = std::make_unique<TextBox>();
    execute_box->set_font(app.default_font());
    TextBox* const execute_box_ptr = execute_box.get();
    std::unique_ptr<Requester> execute = std::make_unique<Requester>(
        app, "Execute Command",
        std::vector<Requester::Action>{{1, U"OK", false, false}, {2, U"Cancel", false, true}});
    execute->set_content(std::move(execute_box));
    execute->set_gadgets(kGadgetClose);
    execute->on_action = [&](uint32_t id) {
        if (id == 1) {
            execute_command(execute_box_ptr->text_utf8());
        }
    };
    execute_box_ptr->on_submit = [&]() {
        execute_command(execute_box_ptr->text_utf8());
        execute->close();
    };

    desktop->on_menu_opened = [&](int menu) {
        /* The open menu's items first, so the runner has them in hand before
         * the cue line that clicks one (specs/testing.md; a step's input is
         * sent the moment its trigger arrives). */
        desktop_ptr->report_parts("bureau");
        Client* const active = registry.active();
        if (active == nullptr) {
            write("  bureau: menu\n");
            return;
        }
        /* Name the client by the opened menu's title. Two clients' menus are
         * then two cues -- "bureau: client menu Ed" and "... Demo" -- which is
         * what the acceptance needs: the string alone is the same for every
         * client, and a second step answering it would race the first. */
        std::u32string const& title =
            (menu >= 0 && menu < static_cast<int>(active->menus.size()))
                ? active->menus[static_cast<size_t>(menu)].title
                : std::u32string();
        write("  bureau: client menu ");
        write(aegir::trinket::utf32_to_utf8(title).c_str());
        write("\n");
    };
    /* The screen layer (specs/workbench.md): an open menu is composited above
     * the windows, so a window over the desktop never hides it. */
    desktop->on_menu_changed = [&](int menu) {
        if (menu < 0) {
            (void)aegir::console::screen_layer(gui, 0, 0, 0, 0);
            return;
        }
        aegir::trinket::Rect const rect = desktop_ptr->open_menu_rect();
        (void)aegir::console::screen_layer(gui, static_cast<uint64_t>(rect.x),
                                           static_cast<uint64_t>(rect.y),
                                           static_cast<uint64_t>(rect.width),
                                           static_cast<uint64_t>(rect.height));
    };
    /* A screen-level shortcut (specs/workbench.md): the console routes a menu
     * accelerator here wherever the focus is, and the menus run the item it
     * names -- Super+Space is Execute. The cue is the acceptance's proof the
     * key crossed to the bureau. */
    app.on_screen_key = [desktop_ptr](KeyEvent const& key) {
        bool const took = desktop_ptr->shortcut(key);
        if (took) {
            write("  bureau: screen shortcut\n");
        }
        return took;
    };
    desktop->on_action = [&](uint32_t action_id) {
        if (action_id == 1) {
            write("  bureau: Aegir, the Workbench\n");
        } else if (action_id == 3) {
            write("  bureau: quit\n");
            aegir::halt();
        } else if (action_id == 8) {
            execute->show();
            /* The requester's focus is set before show() returns -- the focus
             * call blocks until the console has it -- so this cue is race-free:
             * the acceptance types only after reading it, and the keys land in
             * the requester instead of the window focused before it. */
            write("  bureau: execute ready\n");
        } else if (action_id == 4) {
            write("  bureau: clean up\n");
        } else if (action_id == 6) {
            write("  bureau: icons shown\n");
        } else if (action_id == 7) {
            write("  bureau: icons hidden\n");
        }
    };

    /* A click in the active client's menus rings that client's doorbell: the
     * client is already woken by it, drains the console ring, and fetches the
     * action with take_action (specs/workbench.md). The bureau never calls
     * into the client, because a single-threaded client cannot be mid-call
     * and in its event loop at once. */
    desktop->on_client_action = [&](uint32_t action_id) {
        Client* const client = registry.active();
        if (client == nullptr || client->doorbell == 0) return;
        client->pending = action_id;
        seL4_Signal(client->doorbell);
    };

    /* The bureau.menu port, when the director made it -- it does, because the
     * manifest declares it. Serve it: the console's event notification, bound
     * to this thread by exec, wakes the same receive. A boot without the port
     * leaves the bureau with its own menus and no server. */
    uint64_t menu_slot = 0;
    if (aegir::bootstrap::capability("bureau.menu", 11, &menu_slot)) {
        app.serve(aegir::ipc::Owner(static_cast<seL4_CPtr>(menu_slot)));
    }

    app.on_call = [&](uint32_t method, uint64_t const *words, uint32_t count,
                      seL4_Word badge, bool cap_arrived, uint64_t *reply,
                      uint32_t capacity) -> uint32_t {
        (void)capacity;
        if (method == aegir::bureau::menu::kMethodRegister) {
            /* The tree and the doorbell. The cap comes out of the scratch slot
             * whatever the tree does: a second transfer onto an occupied slot
             * fails, so a refused registration must still take its doorbell. A
             * re-registration reuses the client's slot rather than spend a new
             * one (aegir-mem's allocator never frees a slot). */
            Client* const known = registry.find(badge);
            seL4_CPtr const target = (known != nullptr && known->doorbell != 0)
                                         ? known->doorbell
                                         : app.alloc_slot();
            bool have_cap = false;
            if (cap_arrived && target != 0) {
                if (known != nullptr && known->doorbell == target) {
                    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, target,
                                      aegir::bootstrap::kCNodeBits);
                    known->doorbell = 0;
                }
                have_cap = aegir::ipc::take_received_cap(target);
            } else if (cap_arrived) {
                seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                                  aegir::bootstrap::kSlotReceiveCap,
                                  aegir::bootstrap::kCNodeBits);
            }
            std::vector<aegir::bureau::Desktop::Menu> menus;
            if (!have_cap || !aegir::bureau::menu::decode(words, count, &menus)) {
                reply[0] = 0;
                return 1;
            }
            Client& client = registry.ensure(badge);
            client.doorbell = target;
            client.menus = std::move(menus);
            if (Client* const active = registry.active()) {
                desktop_ptr->set_client_menus(active->menus);
            }
            reply[0] = 1;
            return 1;
        }
        if (method == aegir::bureau::menu::kMethodSetActive) {
            bool const on = count >= 1 && words[0] != 0;
            registry.set_active(badge, on);
            if (Client* const active = registry.active()) {
                desktop_ptr->set_client_menus(active->menus);
            } else {
                desktop_ptr->clear_client_menus();
            }
            return 0;
        }
        if (method == aegir::bureau::menu::kMethodTakeAction) {
            Client* const client = registry.find(badge);
            reply[0] = client != nullptr ? client->pending : 0;
            if (client != nullptr) client->pending = 0;
            return 1;
        }
        return 0;
    };

    window.set_content(std::move(desktop));
    window.show();

    /* The screen bar's titles (and an open menu's items) as rect cues, so the
     * acceptance clicks them by name (specs/testing.md). The bar's titles are
     * measured from the font, so their width moves with it. */
    app.on_poll = [desktop_ptr]() { desktop_ptr->report_parts("bureau"); };

    app.on_started = [&]() {
        /* Reserve the screen title bar (specs/workbench.md): the console
         * composites the bar's strip above every window, so nothing covers it.
         * The backdrop is up by now, so the console accepts it. */
        (void)aegir::console::screen_bar(
            gui, static_cast<uint64_t>(desktop_ptr->screen_bar_height()));
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
