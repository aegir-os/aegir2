/*
 * Bureau Window Manager protocol and client API.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_BUREAU_WINDOW_MANAGER_H
#define AEGIR_BUREAU_WINDOW_MANAGER_H

#include <aegir/ipc/port.h>
#include <aegir/trinket/point.h>
#include <aegir/trinket/unicode.h>
#include <cstdint>
#include <functional>
#include <string>

namespace aegir::bureau::wm {

constexpr char kPortName[] = "bureau.wm";
constexpr uint32_t kPortNameLength = 8;

constexpr uint32_t kMethodCreateWindow = 1;
constexpr uint32_t kMethodDestroyWindow = 2;
constexpr uint32_t kMethodSetTitle = 3;
constexpr uint32_t kMethodSetRect = 4;
constexpr uint32_t kMethodGetRect = 5;
constexpr uint32_t kMethodRaise = 6;
constexpr uint32_t kMethodFocus = 7;               // Bureau -> App
constexpr uint32_t kMethodCloseRequest = 8;        // Bureau -> App

struct WindowCreateInfo {
    std::u32string title;
    aegir::trinket::Rect rect;
    bool decorated = true;
    bool resizable = true;
};

struct WindowCreateResult {
    uint64_t client_window_id = 0;   // For damage/listen
    uint64_t frame_window_id = 0;    // Bureau's frame
};

class Client {
public:
    Client();
    ~Client();

    // Create a window (Bureau creates frame + client area)
    bool create_window(const WindowCreateInfo& info, WindowCreateResult& result);

    void destroy_window(uint64_t client_window_id);
    void set_title(uint64_t client_window_id, std::u32string_view title);
    void set_rect(uint64_t client_window_id, const aegir::trinket::Rect& rect);
    void raise(uint64_t client_window_id);

    // Callbacks
    std::function<void(uint64_t, bool)> on_focus;      // gained/lost
    std::function<void(uint64_t)> on_close_request;    // User clicked close

private:
    aegir::ipc::Consumer port_;
    aegir::ipc::Consumer gui_port_;  // For console.gui calls
};

} // namespace aegir::bureau::wm

#endif // AEGIR_BUREAU_WINDOW_MANAGER_H