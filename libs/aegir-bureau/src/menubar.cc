/*
 * Bureau Menubar server implementation.
 */

#include <aegir/bureau/menubar.h>
#include <aegir/bureau/window_manager.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/canvas.h>
#include <aegir/console.h>
#include <aegir/ipc/port.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>
#include <unordered_map>
#include <mutex>

namespace aegir::bureau::menubar {

struct AppState {
    uint64_t app_id;
    std::u32string name;
    MenuTree menu_tree;
    uint64_t console_window_id = 0;  // Menubar window
};

class Server {
public:
    Server() = default;
    ~Server() = default;

    bool start() {
        // Find or create bureau.menu port
        // In real implementation, this port is served by Bureau app
        return true;
    }

    void handle_message(uint32_t method, const uint64_t* words, uint32_t count,
                        uint64_t badge, aegir::ipc::Consumer& reply_port) {
        switch (method) {
            case kMethodRegisterApp:
                handle_register_app(words, count, badge, reply_port);
                break;
            case kMethodUnregisterApp:
                handle_unregister_app(badge, reply_port);
                break;
            case kMethodMenuUpdate:
                handle_menu_update(words, count, badge, reply_port);
                break;
            case kMethodPopupMenu:
                handle_popup_menu(words, count, badge, reply_port);
                break;
            default:
                reply_port.reply(0);
                break;
        }
    }

private:
    std::unordered_map<uint64_t, AppState> apps_;
    std::mutex mutex_;

    void handle_register_app(const uint64_t* words, uint32_t count,
                             uint64_t badge, aegir::ipc::Consumer& reply_port) {
        // Parse app_id, name, and menu tree from words
        // For now, just acknowledge
        reply_port.reply(1);
    }

    void handle_unregister_app(uint64_t badge, aegir::ipc::Consumer& reply_port) {
        reply_port.reply(1);
    }

    void handle_menu_update(const uint64_t* words, uint32_t count,
                            uint64_t badge, aegir::ipc::Consumer& reply_port) {
        // Update menu item
        reply_port.reply(1);
    }

    void handle_popup_menu(const uint64_t* words, uint32_t count,
                           uint64_t badge, aegir::ipc::Consumer& reply_port) {
        reply_port.reply(1);
    }
};

// Client implementation
Client::Client(uint64_t app_id) : app_id_(app_id) {
    port_ = aegir::ipc::Consumer::find(kPortName, kPortNameLength);
}

Client::~Client() = default;

bool Client::register_app(std::u32string_view name, const MenuTree& tree) {
    if (!port_.valid()) return false;

    // Serialize menu tree and send
    // Simplified: just send name and item count
    uint64_t out[4] = {app_id_, 0, 0, 0};  // app_id, name_len, menus_count, items_count
    // TODO: Full serialization

    aegir::ipc::WordsReply reply = port_.call_words(kMethodRegisterApp, out, 4, nullptr, 0);
    return reply.error == 0 && reply.count == 1 && reply.in[0] == 1;
}

bool Client::unregister_app() {
    if (!port_.valid()) return false;
    aegir::ipc::WordsReply reply = port_.call_words(kMethodUnregisterApp, &app_id_, 1, nullptr, 0);
    return reply.error == 0;
}

bool Client::set_item_enabled(uint32_t action_id, bool enabled) {
    if (!port_.valid()) return false;
    uint64_t out[3] = {app_id_, action_id, enabled ? 1 : 0};
    aegir::ipc::WordsReply reply = port_.call_words(kMethodMenuUpdate, out, 3, nullptr, 0);
    return reply.error == 0;
}

bool Client::set_item_checked(uint32_t action_id, bool checked) {
    if (!port_.valid()) return false;
    uint64_t out[3] = {app_id_, action_id, checked ? 1 : 0};
    aegir::ipc::WordsReply reply = port_.call_words(kMethodMenuUpdate, out, 3, nullptr, 0);
    return reply.error == 0;
}

bool Client::set_item_text(uint32_t action_id, std::u32string_view text) {
    if (!port_.valid()) return false;
    // TODO: Serialize text
    return false;
}

bool Client::show_popup_menu(int x, int y, const std::vector<MenuItem>& items) {
    if (!port_.valid()) return false;
    uint64_t out[4] = {app_id_, static_cast<uint64_t>(x), static_cast<uint64_t>(y), 0};
    // TODO: Serialize items
    aegir::ipc::WordsReply reply = port_.call_words(kMethodPopupMenu, out, 4, nullptr, 0);
    return reply.error == 0;
}

} // namespace aegir::bureau::menubar