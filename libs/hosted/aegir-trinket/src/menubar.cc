/*
 * Trinket MenuBar implementation.
 */

#include <aegir/trinket/menubar.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/theme.h>
#include <algorithm>

namespace aegir::trinket {

MenuBar::MenuBar() = default;
MenuBar::~MenuBar() = default;

void MenuBar::set_menus(std::vector<Menu> menus) {
    menus_ = std::move(menus);
    damage();
}

MenuBar::MenuItem* MenuBar::find_item(uint32_t action_id) {
    for (auto& menu : menus_) {
        for (auto& item : menu.items) {
            if (item.action_id == action_id) return &item;
            if (auto* found = find_in_submenu(item.submenu, action_id)) return found;
        }
    }
    return nullptr;
}

const MenuBar::MenuItem* MenuBar::find_item(uint32_t action_id) const {
    for (const auto& menu : menus_) {
        for (const auto& item : menu.items) {
            if (item.action_id == action_id) return &item;
            if (auto* found = find_in_submenu(item.submenu, action_id)) return found;
        }
    }
    return nullptr;
}

MenuBar::MenuItem* MenuBar::find_in_submenu(std::vector<MenuItem>& items, uint32_t action_id) {
    for (auto& item : items) {
        if (item.action_id == action_id) return &item;
        if (auto* found = find_in_submenu(item.submenu, action_id)) return found;
    }
    return nullptr;
}

const MenuBar::MenuItem* MenuBar::find_in_submenu(const std::vector<MenuItem>& items, uint32_t action_id) const {
    for (const auto& item : items) {
        if (item.action_id == action_id) return &item;
        if (auto* found = find_in_submenu(item.submenu, action_id)) return found;
    }
    return nullptr;
}

void MenuBar::set_item_enabled(uint32_t action_id, bool enabled) {
    if (auto* item = find_item(action_id)) {
        if (enabled) item->flags &= ~MenuItem::Flags::DISABLED;
        else item->flags |= MenuItem::Flags::DISABLED;
        damage();
    }
}

void MenuBar::set_item_checked(uint32_t action_id, bool checked) {
    if (auto* item = find_item(action_id)) {
        if (checked) item->flags |= MenuItem::Flags::CHECKED;
        else item->flags &= ~MenuItem::Flags::CHECKED;
        damage();
    }
}

void MenuBar::set_item_text(uint32_t action_id, std::u32string_view text) {
    if (auto* item = find_item(action_id)) {
        item->label = std::u32string(text);
        damage();
    }
}

void MenuBar::show_context_menu(Point global_pos, const std::vector<MenuItem>& items) {
    state_.popup_pos = global_pos;
    state_.popup_items = items;
    damage();
}

void MenuBar::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);

    Theme& theme = Application::instance()->theme();
    Rect r = rect_;

    canvas.fill_rect(r, theme.color(ColorRole::MENUBAR_BG));

    int x = r.x;
    for (const auto& menu : menus_) {
        Font* font = Application::instance()->default_font();
        if (!font) continue;

        int y = r.y + (r.height + font->ascent() - font->descent()) / 2;
        Color text_color = theme.color(ColorRole::MENUBAR_TEXT);

        // Highlight hovered menu
        if (state_.hover_index >= 0 && state_.hover_index < static_cast<int>(menus_.size())) {
            // TODO: Highlight
        }

        canvas.draw_text({x + 12, y}, menu.title, font, text_color);
        x += static_cast<int>(font->measure(menu.title).width) + 24;
    }

    // Draw open menu
    if (state_.open_index >= 0 && state_.open_index < static_cast<int>(menus_.size())) {
        draw_menu(canvas, menus_[state_.open_index], state_.open_index);
    }

    // Draw popup menu
    if (!state_.popup_items.empty()) {
        draw_popup(canvas, state_.popup_pos, state_.popup_items);
    }
}

void MenuBar::draw_menu(Canvas& canvas, const Menu& menu, int index) {
    Theme& theme = Application::instance()->theme();
    Font* font = Application::instance()->default_font();
    if (!font) return;

    int item_height = theme.metric(MetricRole::MENU_ITEM_HEIGHT);
    int padding_h = theme.metric(MetricRole::MENU_PADDING_H);

    // Calculate menu position (below menubar)
    int menu_x = rect_.x;
    for (int i = 0; i < index; ++i) {
        menu_x += static_cast<int>(font->measure(menus_[i].title).width) + 24;
    }
    int menu_y = rect_.y + rect_.height;

    // Calculate width
    int max_width = 200;
    for (const auto& item : menu.items) {
        Size sz = font->measure(item.label);
        max_width = std::max(max_width, sz.width + 2 * padding_h + 32);
    }

    int menu_height = static_cast<int>(menu.items.size()) * item_height;
    Rect menu_rect = {menu_x, menu_y, max_width, menu_height};

    canvas.fill_rect(menu_rect, theme.color(ColorRole::MENU_BG));
    canvas.draw_rect(menu_rect, theme.color(ColorRole::MENU_BORDER), theme.metric(MetricRole::MENU_BORDER_WIDTH));

    int item_y = menu_y;
    for (size_t i = 0; i < menu.items.size(); ++i) {
        const auto& item = menu.items[i];
        Rect item_rect = {menu_x, item_y, max_width, item_height};

        if (item.flags & MenuItem::SEPARATOR) {
            int sep_y = item_y + item_height / 2;
            canvas.draw_hline(menu_x + 10, menu_x + max_width - 10, sep_y, theme.color(ColorRole::MENU_SEPARATOR));
        } else {
            bool hovered = (state_.submenu_index == static_cast<int>(i));
            if (hovered && !(item.flags & MenuItem::DISABLED)) {
                canvas.fill_rect(item_rect, theme.color(ColorRole::MENU_HOVER));
            }

            Color text_color = (item.flags & MenuItem::DISABLED) ? theme.color(ColorRole::DISABLED_TEXT)
                                                                : theme.color(ColorRole::MENU_TEXT);

            int text_x = menu_x + padding_h;
            int text_y = item_y + (item_height + font->ascent() - font->descent()) / 2;
            canvas.draw_text({text_x, text_y}, item.label, font, text_color);

            // Shortcut
            if (item.shortcut_key != KeyCode::UNKNOWN) {
                std::u32string shortcut;
                if (item.shortcut_mods & 2) shortcut += U"Ctrl+";
                if (item.shortcut_mods & 1) shortcut += U"Shift+";
                if (item.shortcut_mods & 4) shortcut += U"Alt+";
                shortcut += keycode_to_string(item.shortcut_key);

                Size sc_sz = font->measure(shortcut);
                int sc_x = menu_x + max_width - padding_h - sc_sz.width;
                canvas.draw_text({sc_x, text_y}, shortcut, font, text_color);
            }

            // Checkmark
            if (item.flags & MenuItem::CHECKED) {
                int cx = menu_x + 8;
                int cy = item_y + item_height / 2;
                canvas.draw_line({cx, cy}, {cx + 4, cy + 4}, text_color);
                canvas.draw_line({cx + 4, cy + 4}, {cx + 10, cy - 4}, text_color);
            }

            // Submenu indicator
            if (item.flags & MenuItem::SUBMENU) {
                int ax = menu_x + max_width - padding_h - 10;
                int ay = item_y + item_height / 2;
                canvas.draw_line({ax, ay - 4}, {ax + 6, ay}, text_color);
                canvas.draw_line({ax + 6, ay}, {ax, ay + 4}, text_color);
            }
        }

        item_y += item_height;
    }
}

void MenuBar::draw_popup(Canvas& canvas, Point pos, const std::vector<MenuItem>& items) {
    static_cast<void>(canvas);
    static_cast<void>(pos);
    static_cast<void>(items);
    // Similar to draw_menu but at arbitrary position
}

void MenuBar::on_mouse_down(const MouseEvent& event) {
    if (!enabled_) return;

    // Check if clicking on a menu title
    Font* font = Application::instance()->default_font();
    if (!font) return;

    int x = rect_.x;
    for (size_t i = 0; i < menus_.size(); ++i) {
        Size sz = font->measure(menus_[i].title);
        if (event.pos.x >= x && event.pos.x < x + static_cast<int>(sz.width) + 24 &&
            event.pos.y >= rect_.y && event.pos.y < rect_.y + rect_.height) {
            if (state_.open_index == static_cast<int>(i)) {
                state_.open_index = -1;
            } else {
                state_.open_index = static_cast<int>(i);
            }
            damage();
            return;
        }
        x += static_cast<int>(sz.width) + 24;
    }
}

void MenuBar::on_mouse_up(const MouseEvent& event) {
    if (!enabled_) return;

    // Check if clicking on an open menu item
    if (state_.open_index >= 0 && state_.open_index < static_cast<int>(menus_.size())) {
        const auto& menu = menus_[state_.open_index];
        Font* f = Application::instance()->default_font();
        if (!f) return;

        int item_height = Application::instance()->theme().metric(MetricRole::MENU_ITEM_HEIGHT);
        int menu_x = rect_.x;
        for (int i = 0; i < state_.open_index; ++i) {
            menu_x += static_cast<int>(f->measure(menus_[i].title).width) + 24;
        }
        int menu_y = rect_.y + rect_.height;

        int item_y = menu_y;
        for (size_t i = 0; i < menu.items.size(); ++i) {
            Rect item_rect = {menu_x, item_y,
                              f->measure(menu.items[i].label).width + 2 * Application::instance()->theme().metric(MetricRole::MENU_PADDING_H) + 32,
                              item_height};
            if (item_rect.contains(event.pos)) {
                const auto& item = menu.items[i];
                if (!(item.flags & MenuItem::DISABLED) && !(item.flags & MenuItem::SEPARATOR)) {
                    if (item.flags & MenuItem::SUBMENU) {
                        state_.submenu_index = static_cast<int>(i);
                    } else if (on_action) {
                        on_action(item.action_id);
                    }
                }
                state_.open_index = -1;
                damage();
                return;
            }
            item_y += item_height;
        }
    }

    state_.open_index = -1;
    damage();
}

void MenuBar::on_mouse_move(const MouseEvent& event) {
    if (!enabled_) return;

    // Hover detection for menu titles
    Font* font = Application::instance()->default_font();
    if (!font) return;

    int x = rect_.x;
    bool any_hover = false;
    for (size_t i = 0; i < menus_.size(); ++i) {
        Size sz = font->measure(menus_[i].title);
        if (event.pos.x >= x && event.pos.x < x + static_cast<int>(sz.width) + 24 &&
            event.pos.y >= rect_.y && event.pos.y < rect_.y + rect_.height) {
            if (state_.hover_index != static_cast<int>(i)) {
                state_.hover_index = static_cast<int>(i);
                any_hover = true;
                damage();
            }
        }
        x += static_cast<int>(sz.width) + 24;
    }
    if (!any_hover && state_.hover_index >= 0) {
        state_.hover_index = -1;
        damage();
    }
}

void MenuBar::on_mouse_leave(const MouseEvent&) {
    if (state_.hover_index >= 0) {
        state_.hover_index = -1;
        damage();
    }
}

void MenuBar::on_key_down(const KeyEvent& event) {
    if (event.code == KeyCode::ESCAPE) {
        state_.open_index = -1;
        state_.submenu_index = -1;
        damage();
    }
}

Size MenuBar::preferred_size() const {
    Font* font = Application::instance()->default_font();
    if (!font) return {0, Application::instance()->theme().metric(MetricRole::MENUBAR_HEIGHT)};

    int width = 0;
    for (const auto& menu : menus_) {
        width += static_cast<int>(font->measure(menu.title).width) + 24;
    }
    return {width, Application::instance()->theme().metric(MetricRole::MENUBAR_HEIGHT)};
}

std::u32string MenuBar::keycode_to_string(KeyCode code) {
    switch (code) {
        case KeyCode::F1: return U"F1";
        case KeyCode::F2: return U"F2";
        case KeyCode::F3: return U"F3";
        case KeyCode::F4: return U"F4";
        case KeyCode::F5: return U"F5";
        case KeyCode::F6: return U"F6";
        case KeyCode::F7: return U"F7";
        case KeyCode::F8: return U"F8";
        case KeyCode::F9: return U"F9";
        case KeyCode::F10: return U"F10";
        case KeyCode::F11: return U"F11";
        case KeyCode::F12: return U"F12";
        default: return std::u32string(1, static_cast<char32_t>(code));
    }
}

} // namespace aegir::trinket