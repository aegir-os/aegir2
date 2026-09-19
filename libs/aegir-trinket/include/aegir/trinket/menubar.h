/*
 * Trinket MenuBar widget.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_MENUBAR_H
#define AEGIR_TRINKET_MENUBAR_H

#include <aegir/trinket/widget.h>
#include <aegir/trinket/unicode.h>
#include <functional>
#include <string>
#include <vector>

namespace aegir::trinket {

class Menu;
class MenuItem;

class MenuBar : public Widget {
public:
    MenuBar();
    ~MenuBar() override;

    struct Menu {
        std::u32string title;
        std::vector<MenuItem> items;
    };

    struct MenuItem {
        uint32_t action_id = 0;
        std::u32string label;
        KeyCode shortcut_key = KeyCode::UNKNOWN;
        uint32_t shortcut_mods = 0;
        enum Flags : uint8_t {
            NONE = 0,
            CHECKED = 1,
            DISABLED = 2,
            SEPARATOR = 4,
            SUBMENU = 8,
            RADIO = 16
        };
        uint8_t flags = NONE;
        std::vector<MenuItem> submenu;
    };

    void set_menus(std::vector<Menu> menus);
    const std::vector<Menu>& menus() const { return menus_; }

    // Find menu item by action_id
    MenuItem* find_item(uint32_t action_id);
    const MenuItem* find_item(uint32_t action_id) const;

    // Enable/disable/check items
    void set_item_enabled(uint32_t action_id, bool enabled);
    void set_item_checked(uint32_t action_id, bool checked);
    void set_item_text(uint32_t action_id, std::u32string_view text);

    // Called when a menu item is activated
    std::function<void(uint32_t action_id)> on_action;

    // Show context menu at position (for right-click)
    void show_context_menu(Point global_pos, const std::vector<MenuItem>& items);

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    void on_mouse_down(const MouseEvent& event) override;
    void on_mouse_up(const MouseEvent& event) override;
    void on_mouse_move(const MouseEvent& event) override;
    void on_mouse_leave(const MouseEvent& event) override;
    void on_key_down(const KeyEvent& event) override;
    Size preferred_size() const override;

private:
    struct MenuState {
        int hover_index = -1;
        int open_index = -1;  // Which top-level menu is open
        int submenu_index = -1;
        Point popup_pos;
        std::vector<MenuItem> popup_items;
    };
    std::vector<Menu> menus_;
    MenuState state_;

    // Popup menu handling
    void open_menu(int index, const Point& pos);
    void close_menu();
    void handle_popup_click(const MouseEvent& event);
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_MENUBAR_H