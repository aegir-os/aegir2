/*
 * Bureau Menubar protocol and client API.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_BUREAU_MENUBAR_H
#define AEGIR_BUREAU_MENUBAR_H

#include <aegir/ipc/port.h>
#include <aegir/trinket/unicode.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aegir::bureau::menubar {

constexpr char kPortName[] = "bureau.menu";
constexpr uint32_t kPortNameLength = 11;

// Method numbers
constexpr uint32_t kMethodRegisterApp = 1;
constexpr uint32_t kMethodUnregisterApp = 2;
constexpr uint32_t kMethodMenuAction = 3;          // Bureau -> App
constexpr uint32_t kMethodMenuUpdate = 4;          // App -> Bureau
constexpr uint32_t kMethodFocusChanged = 5;        // Bureau -> App
constexpr uint32_t kMethodPopupMenu = 6;           // App -> Bureau

// MenuItem flags
enum ItemFlags : uint8_t {
    NONE = 0,
    CHECKED = 1,
    DISABLED = 2,
    SEPARATOR = 4,
    SUBMENU = 8,
    RADIO = 16
};

struct MenuItem {
    uint32_t action_id = 0;
    std::u32string label;
    uint32_t shortcut_key = 0;   // Raw key code
    uint32_t shortcut_mods = 0;  // SHIFT=1, CTRL=2, ALT=4, META=8
    ItemFlags flags = NONE;
    std::vector<MenuItem> submenu;
};

struct Menu {
    std::u32string title;
    std::vector<MenuItem> items;
};

struct MenuTree {
    std::vector<Menu> menus;
};

// Client API
class Client {
public:
    explicit Client(uint64_t app_id);
    ~Client();

    // Register this app's menu tree
    bool register_app(std::u32string_view name, const MenuTree& tree);
    bool unregister_app();

    // Update individual items
    bool set_item_enabled(uint32_t action_id, bool enabled);
    bool set_item_checked(uint32_t action_id, bool checked);
    bool set_item_text(uint32_t action_id, std::u32string_view text);

    // Request popup menu at position
    bool show_popup_menu(int x, int y, const std::vector<MenuItem>& items);

    // Callbacks (set by app)
    std::function<void(uint32_t action_id)> on_action;
    std::function<void(bool)> on_focus_changed;

private:
    uint64_t app_id_;
    aegir::ipc::Consumer port_;
};

} // namespace aegir::bureau::menubar

#endif // AEGIR_BUREAU_MENUBAR_H