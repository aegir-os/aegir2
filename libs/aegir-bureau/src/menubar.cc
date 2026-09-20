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

// Client implementation
Client::Client(uint64_t app_id) : app_id_(app_id) {
    port_ = aegir::ipc::Consumer::find(kPortName, kPortNameLength);
}

Client::~Client() = default;

bool Client::register_app(std::u32string_view name, const MenuTree& tree) {
    static_cast<void>(name);
    static_cast<void>(tree);  // menu-tree serialization is the menu arc's
    if (!port_.valid()) return false;

    // Serialize menu tree and send
    // Simplified: just send name and item count
    uint64_t out[4] = {app_id_, 0, 0, 0};  // app_id, name_len, menus_count, items_count
    // TODO: Full serialization

    uint64_t in[1] = {0};
    aegir::ipc::WordsReply const reply = port_.call_words(kMethodRegisterApp, out, 4, in, 1);
    return reply.error == 0 && reply.count == 1 && in[0] == 1;
}

bool Client::unregister_app() {
    if (!port_.valid()) return false;
    aegir::ipc::WordsReply reply = port_.call_words(kMethodUnregisterApp, &app_id_, 1, nullptr, 0);
    return reply.error == 0;
}

bool Client::set_item_enabled(uint32_t action_id, bool enabled) {
    if (!port_.valid()) return false;
    uint64_t out[3] = {app_id_, action_id, static_cast<uint64_t>(enabled ? 1 : 0)};
    aegir::ipc::WordsReply const reply = port_.call_words(kMethodMenuUpdate, out, 3, nullptr, 0);
    return reply.error == 0;
}

bool Client::set_item_checked(uint32_t action_id, bool checked) {
    if (!port_.valid()) return false;
    uint64_t out[3] = {app_id_, action_id, static_cast<uint64_t>(checked ? 1 : 0)};
    aegir::ipc::WordsReply const reply = port_.call_words(kMethodMenuUpdate, out, 3, nullptr, 0);
    return reply.error == 0;
}

bool Client::set_item_text(uint32_t action_id, std::u32string_view text) {
    static_cast<void>(action_id);
    static_cast<void>(text);  // text serialization is the menu arc's
    if (!port_.valid()) return false;
    // TODO: Serialize text
    return false;
}

bool Client::show_popup_menu(int x, int y, const std::vector<MenuItem>& items) {
    static_cast<void>(items);  // item serialization is the menu arc's
    if (!port_.valid()) return false;
    uint64_t out[4] = {app_id_, static_cast<uint64_t>(x), static_cast<uint64_t>(y), 0};
    // TODO: Serialize items
    aegir::ipc::WordsReply reply = port_.call_words(kMethodPopupMenu, out, 4, nullptr, 0);
    return reply.error == 0;
}

} // namespace aegir::bureau::menubar