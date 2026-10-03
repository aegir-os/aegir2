/*
 * The Workbench screen implementation. See desktop.h and specs/workbench.md.
 */

#include <aegir/bureau/desktop.h>

#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/diagnostics.h>
#include <aegir/trinket/font.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/unicode.h>

#include <algorithm>

namespace aegir::bureau {

using aegir::trinket::Application;
using aegir::trinket::Canvas;
using aegir::trinket::Color;
using aegir::trinket::ColorRole;
using aegir::trinket::Font;
using aegir::trinket::KeyCode;
using aegir::trinket::KeyEvent;
using aegir::trinket::MenuBar;
using aegir::trinket::MetricRole;
using aegir::trinket::MouseEvent;
using aegir::trinket::PaintEvent;
using aegir::trinket::Rect;
using aegir::trinket::Size;
using aegir::trinket::Theme;

Desktop::Desktop() = default;
Desktop::~Desktop() = default;

void Desktop::set_menus(std::vector<Menu> menus) {
    menus_ = std::move(menus);
    set_open_menu(-1);
    damage();
}

void Desktop::set_client_menus(std::vector<Menu> menus) {
    client_menus_ = std::move(menus);
    client_active_ = true;
    set_open_menu(-1);
    damage();
}

void Desktop::clear_client_menus() {
    client_active_ = false;
    client_menus_.clear();
    set_open_menu(-1);
    damage();
}

std::vector<Desktop::Menu> const& Desktop::active_menus() const {
    return client_active_ ? client_menus_ : menus_;
}

void Desktop::set_open_menu(int menu) {
    if (open_menu_ == menu) {
        return;
    }
    open_menu_ = menu;
    if (on_menu_changed) on_menu_changed(menu);
}

int Desktop::screen_bar_height() const {
    return bar_height();
}

Rect Desktop::open_menu_rect() const {
    if (open_menu_ < 0 || open_menu_ >= static_cast<int>(active_menus().size())) {
        return Rect{0, 0, 0, 0};
    }
    std::vector<Slot> const slots = item_slots(open_menu_);
    if (slots.empty()) {
        return Rect{0, 0, 0, 0};
    }
    int const height = item_height();
    return Rect{slots.front().rect.x, slots.front().rect.y, slots.front().rect.width,
                static_cast<int>(active_menus()[open_menu_].items.size()) * height};
}

bool Desktop::shortcut(KeyEvent const& key) {
    if (!key.pressed || key.code == KeyCode::UNKNOWN) {
        return false;
    }
    std::vector<Menu> const& menus = active_menus();
    for (Menu const& menu : menus) {
        for (MenuItem const& item : menu.items) {
            if (item.shortcut_key == KeyCode::UNKNOWN || item.shortcut_key != key.code ||
                item.shortcut_mods != key.modifiers) {
                continue;
            }
            if ((item.flags & (MenuItem::DISABLED | MenuItem::SEPARATOR)) != 0) {
                return false;
            }
            set_open_menu(-1);
            damage();
            if (client_active_) {
                if (on_client_action) on_client_action(item.action_id);
            } else if (on_action) {
                on_action(item.action_id);
            }
            return true;
        }
    }
    return false;
}

int Desktop::bar_height() const {
    return Application::instance()->theme().metric(MetricRole::MENUBAR_HEIGHT);
}

int Desktop::item_height() const {
    return Application::instance()->theme().metric(MetricRole::MENU_ITEM_HEIGHT);
}

int Desktop::menu_width(int menu) const {
    Theme& theme = Application::instance()->theme();
    Font* const font = Application::instance()->default_font();
    int const pad = theme.metric(MetricRole::MENU_PADDING_H);
    int width = 120;
    for (const MenuItem& item : active_menus()[menu].items) {
        int const label = font != nullptr ? font->measure(item.label).width : 0;
        int const accel = item.shortcut_key != KeyCode::UNKNOWN
                              ? MenuBar::accelerator_width(item, font) + 24
                              : 0;
        width = std::max(width, label + 2 * pad + 24 + accel);
    }
    return width;
}

std::vector<Desktop::Slot> Desktop::title_slots() const {
    std::vector<Slot> slots;
    Font* const font = Application::instance()->default_font();
    if (font == nullptr) return slots;
    Theme& theme = Application::instance()->theme();
    int const pad = theme.metric(MetricRole::MENU_PADDING_H);
    int const height = bar_height();
    int x = rect_.x;
    std::vector<Menu> const& menus = active_menus();
    for (int i = 0; i < static_cast<int>(menus.size()); ++i) {
        int const width = font->measure(menus[i].title).width + 2 * pad;
        slots.push_back({Rect{x, rect_.y, width, height}, i, 0});
        x += width;
    }
    return slots;
}

std::vector<Desktop::Slot> Desktop::item_slots(int menu) const {
    std::vector<Slot> slots;
    std::vector<Slot> const titles = title_slots();
    if (menu < 0 || menu >= static_cast<int>(titles.size())) return slots;
    int const height = item_height();
    int const x = titles[menu].rect.x;
    int const width = menu_width(menu);
    int y = rect_.y + bar_height();
    for (int i = 0; i < static_cast<int>(active_menus()[menu].items.size()); ++i) {
        slots.push_back({Rect{x, y, width, height}, menu, i});
        y += height;
    }
    return slots;
}

void Desktop::report_parts(char const *prefix) const
{
    std::vector<Slot> const titles = title_slots();
    for (size_t i = 0; i < titles.size(); ++i) {
        report_rect_indexed(prefix, "title", static_cast<int>(i) + 1,
                            screen_rect_of(*this, titles[i].rect));
    }
    if (open_menu_ < 0) return;
    std::vector<Slot> const items = item_slots(open_menu_);
    for (size_t i = 0; i < items.size(); ++i) {
        report_rect_indexed(prefix, "item", static_cast<int>(i) + 1,
                            screen_rect_of(*this, items[i].rect));
    }
}

void Desktop::draw_bar(Canvas& canvas) {
    Theme& theme = Application::instance()->theme();
    int const height = bar_height();
    Rect const bar{rect_.x, rect_.y, rect_.width, height};
    /* The screen bar's face is the theme's (specs/trinket/chrome.md); the
     * desktop draws the titles it knows. */
    theme.draw_screen_bar(canvas, bar);

    Font* const font = Application::instance()->default_font();
    if (font == nullptr) return;
    int const pad = theme.metric(MetricRole::MENU_PADDING_H);
    for (const Slot& slot : title_slots()) {
        int const x = slot.rect.x + pad;
        int const y = slot.rect.y + (height - font->height()) / 2;
        canvas.draw_text({x, y}, active_menus()[slot.menu].title, font,
                         theme.color(ColorRole::TITLEBAR_TEXT));
    }
}

void Desktop::draw_menu(Canvas& canvas, int menu) {
    Theme& theme = Application::instance()->theme();
    Font* const font = Application::instance()->default_font();
    if (font == nullptr) return;
    int const height = item_height();
    int const pad = theme.metric(MetricRole::MENU_PADDING_H);
    std::vector<Slot> const slots = item_slots(menu);
    if (slots.empty()) return;

    Rect const box{slots.front().rect.x, slots.front().rect.y,
                   slots.front().rect.width,
                   static_cast<int>(active_menus()[menu].items.size()) * height};
    /* The well and each row are the theme's (specs/trinket/chrome.md); the
     * desktop places the rows and draws the accelerators. */
    theme.draw_menu_well(canvas, box);

    for (const Slot& slot : slots) {
        MenuItem const& item = active_menus()[menu].items[slot.item];
        theme.draw_menu_item(canvas, slot.rect,
                             aegir::trinket::utf32_to_utf8(item.label).c_str(),
                             false,
                             (item.flags & MenuItem::CHECKED) != 0,
                             (item.flags & MenuItem::DISABLED) != 0,
                             (item.flags & MenuItem::SEPARATOR) != 0,
                             (item.flags & MenuItem::SUBMENU) != 0);
        if (item.shortcut_key != KeyCode::UNKNOWN) {
            Color const text = (item.flags & MenuItem::DISABLED) != 0
                                   ? theme.color(ColorRole::DISABLED_TEXT)
                                   : theme.color(ColorRole::MENU_TEXT);
            MenuBar::draw_accelerator(canvas, slot.rect.x + slot.rect.width - pad,
                                      slot.rect.y + (height - font->height()) / 2, item,
                                      font, text);
        }
    }
}

void Desktop::on_paint(Canvas& canvas, const PaintEvent&) {
    Theme& theme = Application::instance()->theme();
    canvas.fill_rect(rect_, theme.color(ColorRole::BACKGROUND));
    draw_bar(canvas);
    if (open_menu_ >= 0 && open_menu_ < static_cast<int>(active_menus().size())) {
        draw_menu(canvas, open_menu_);
    }
}

void Desktop::on_mouse_down(const MouseEvent& event) {
    for (const Slot& slot : title_slots()) {
        if (slot.rect.contains(event.pos)) {
            bool const opening = open_menu_ != slot.menu;
            set_open_menu(opening ? slot.menu : -1);
            damage();
            if (opening && on_menu_opened) on_menu_opened(open_menu_);
            return;
        }
    }
    if (open_menu_ >= 0) {
        for (const Slot& slot : item_slots(open_menu_)) {
            if (!slot.rect.contains(event.pos)) continue;
            MenuItem const& item = active_menus()[open_menu_].items[slot.item];
            uint32_t const action = item.action_id;
            bool const acts = (item.flags & (MenuItem::DISABLED | MenuItem::SEPARATOR)) == 0;
            bool const client = client_active_;
            set_open_menu(-1);
            damage();
            if (acts) {
                if (client) {
                    if (on_client_action) on_client_action(action);
                } else if (on_action) {
                    on_action(action);
                }
            }
            return;
        }
        set_open_menu(-1);
        damage();
    }
}

Size Desktop::preferred_size() const {
    return rect_.size();
}

} // namespace aegir::bureau
