/*
 * Bureau Window Manager server implementation.
 */

#include <aegir/bureau/window_manager.h>
#include <aegir/bureau/menubar.h>
#include <aegir/trinket/application.h>
#include <aegir/console.h>
#include <aegir/ipc/port.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>
#include <unordered_map>
#include <mutex>

namespace aegir::bureau::wm {

// Client implementation
Client::Client() {
    port_ = aegir::ipc::Consumer::find(kPortName, kPortNameLength);
    if (aegir::trinket::Application::instance()) {
        gui_port_ = aegir::trinket::Application::instance()->gui_port();
    }
}

Client::~Client() = default;

bool Client::create_window(const WindowCreateInfo& info, WindowCreateResult& result) {
    if (!port_.valid() || !gui_port_.valid()) return false;

    uint64_t out[5] = {
        static_cast<uint64_t>(info.rect.x),
        static_cast<uint64_t>(info.rect.y),
        static_cast<uint64_t>(info.rect.width),
        static_cast<uint64_t>(info.rect.height),
        static_cast<uint64_t>(info.decorated ? 1 : 0)
    };
    // TODO: Send title

    uint64_t in[2] = {0, 0};
    aegir::ipc::WordsReply const reply = port_.call_words(kMethodCreateWindow, out, 5, in, 2);
    if (reply.error == 0 && reply.count == 2) {
        result.client_window_id = in[0];
        result.frame_window_id = in[1];
        return true;
    }
    return false;
}

void Client::destroy_window(uint64_t client_window_id) {
    if (!port_.valid()) return;
    uint64_t out[2] = {0, client_window_id};
    port_.call_words(kMethodDestroyWindow, out, 2, nullptr, 0);
}

void Client::set_title(uint64_t client_window_id, std::u32string_view title) {
    static_cast<void>(client_window_id);
    static_cast<void>(title);  // title serialization is the WM arc's
    if (!port_.valid()) return;
    // TODO
}

void Client::set_rect(uint64_t client_window_id, const aegir::trinket::Rect& rect) {
    if (!port_.valid()) return;
    uint64_t out[5] = {0, client_window_id,
                       static_cast<uint64_t>(rect.x),
                       static_cast<uint64_t>(rect.y),
                       static_cast<uint64_t>(rect.width)};
    // TODO: Send height
    port_.call_words(kMethodSetRect, out, 5, nullptr, 0);
}

void Client::raise(uint64_t client_window_id) {
    if (!port_.valid()) return;
    uint64_t out[2] = {0, client_window_id};
    port_.call_words(kMethodRaise, out, 2, nullptr, 0);
}

} // namespace aegir::bureau::wm