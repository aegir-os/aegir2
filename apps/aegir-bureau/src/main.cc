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
