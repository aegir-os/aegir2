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

struct WindowState {
    uint64_t client_window_id;
    uint64_t frame_window_id;
    uint64_t app_badge;
    aegir::trinket::Rect rect;
    std::u32string title;
    bool decorated;
    bool resizable;
    bool focused = false;
};

class Server {
public:
    Server() = default;
    ~Server() = default;

    bool start() {
        // Bureau serves bureau.wm port
        return true;
    }

    void handle_message(uint32_t method, const uint64_t* words, uint32_t count,
                        uint64_t badge, aegir::ipc::Consumer& reply_port) {
        switch (method) {
            case kMethodCreateWindow:
                handle_create_window(words, count, badge, reply_port);
                break;
            case kMethodDestroyWindow:
                handle_destroy_window(words, count, badge, reply_port);
                break;
            case kMethodSetTitle:
                handle_set_title(words, count, badge, reply_port);
                break;
            case kMethodSetRect:
                handle_set_rect(words, count, badge, reply_port);
                break;
            case kMethodRaise:
                handle_raise(words, count, badge, reply_port);
                break;
            default:
                reply_port.reply(0);
                break;
        }
    }

    void notify_focus(uint64_t client_window_id, bool gained) {
        auto it = windows_.find(client_window_id);
        if (it != windows_.end()) {
            it->second.focused = gained;
            // Send focus notification to app
            // TODO: Send via bureau.wm focus method
        }
    }

private:
    std::unordered_map<uint64_t, WindowState> windows_;
    std::mutex mutex_;
    uint64_t next_client_id_ = 1;

    void handle_create_window(const uint64_t* words, uint32_t count,
                              uint64_t badge, aegir::ipc::Consumer& reply_port) {
        // Parse WindowCreateInfo
        // For now, just create a console window
        if (!aegir::trinket::Application::instance() ||
            !aegir::trinket::Application::instance()->gui_port().valid()) {
            reply_port.reply(0);
            return;
        }

        auto& app = *aegir::trinket::Application::instance();
        aegir::ipc::Consumer& gui = app.gui_port();

        // Extract rect from words (simplified)
        aegir::trinket::Rect rect = {
            static_cast<int>(words[1]),
            static_cast<int>(words[2]),
            static_cast<int>(words[3]),
            static_cast<int>(words[4])
        };

        uint64_t console_win = aegir::console::create_window(
            gui, rect.x, rect.y, rect.width, rect.height, 0, 0);

        if (console_win == 0) {
            reply_port.reply(0);
            return;
        }

        uint64_t client_id = next_client_id_++;
        windows_[client_id] = {client_id, 0, badge, rect, U"", true, true, false};

        uint64_t out[2] = {client_id, 0};  // client_window_id, frame_window_id
        reply_port.reply_words(out, 2);
    }

    void handle_destroy_window(const uint64_t* words, uint32_t count,
                               uint64_t badge, aegir::ipc::Consumer& reply_port) {
        uint64_t client_id = words[1];
        auto it = windows_.find(client_id);
        if (it != windows_.end()) {
            if (it->second.client_window_id) {
                aegir::console::destroy_window(
                    aegir::trinket::Application::instance()->gui_port(),
                    it->second.client_window_id);
            }
            windows_.erase(it);
            reply_port.reply(1);
        } else {
            reply_port.reply(0);
        }
    }

    void handle_set_title(const uint64_t* words, uint32_t count,
                          uint64_t badge, aegir::ipc::Consumer& reply_port) {
        // TODO
        reply_port.reply(1);
    }

    void handle_set_rect(const uint64_t* words, uint32_t count,
                         uint64_t badge, aegir::ipc::Consumer& reply_port) {
        // TODO
        reply_port.reply(1);
    }

    void handle_raise(const uint64_t* words, uint32_t count,
                      uint64_t badge, aegir::ipc::Consumer& reply_port) {
        // TODO
        reply_port.reply(1);
    }

    std::unordered_map<uint64_t, WindowState> windows_;
    uint64_t next_client_id_ = 1;
};

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
        info.decorated ? 1 : 0
    };
    // TODO: Send title

    aegir::ipc::WordsReply reply = port_.call_words(kMethodCreateWindow, out, 5, nullptr, 0);
    if (reply.error == 0 && reply.count == 2) {
        result.client_window_id = reply.in[0];
        result.frame_window_id = reply.in[1];
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