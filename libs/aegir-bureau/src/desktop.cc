/*
 * The Workbench screen implementation. See desktop.h and specs/workbench.md.
 */

#include <aegir/bureau/desktop.h>

#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/font.h>
#include <aegir/trinket/theme.h>

#include <algorithm>

namespace aegir::bureau {

using aegir::trinket::Application;
using aegir::trinket::Canvas;
using aegir::trinket::Color;
using aegir::trinket::ColorRole;
using aegir::trinket::Font;
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
    open_menu_ = -1;
    damage();
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
    for (const MenuItem& item : menus_[menu].items) {
        int const label = font != nullptr ? font->measure(item.label).width : 0;
        width = std::max(width, label + 2 * pad + 24);
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
    for (int i = 0; i < static_cast<int>(menus_.size()); ++i) {
        int const width = font->measure(menus_[i].title).width + 2 * pad;
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
    for (int i = 0; i < static_cast<int>(menus_[menu].items.size()); ++i) {
        slots.push_back({Rect{x, y, width, height}, menu, i});
        y += height;
    }
    return slots;
}

void Desktop::draw_bar(Canvas& canvas) {
    Theme& theme = Application::instance()->theme();
    int const height = bar_height();
    Rect const bar{rect_.x, rect_.y, rect_.width, height};
    canvas.fill_rect(bar, theme.color(ColorRole::TITLEBAR_BG));
    if (height > 2) {
        canvas.draw_hline(bar.x, bar.x + bar.width - 1, bar.y + 1,
                          theme.color(ColorRole::TITLEBAR_HIGHLIGHT));
    }
    canvas.draw_hline(bar.x, bar.x + bar.width - 1, bar.y + height - 1,
                      theme.color(ColorRole::TITLEBAR_SHADOW));

    Font* const font = Application::instance()->default_font();
    if (font == nullptr) return;
    int const pad = theme.metric(MetricRole::MENU_PADDING_H);
    for (const Slot& slot : title_slots()) {
        int const x = slot.rect.x + pad;
        int const y = slot.rect.y + (height - font->height()) / 2;
        canvas.draw_text({x, y}, menus_[slot.menu].title, font,
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
                   static_cast<int>(menus_[menu].items.size()) * height};
    canvas.fill_rect(box, theme.color(ColorRole::MENU_BG));
    canvas.draw_rect(box, theme.color(ColorRole::MENU_BORDER),
                     theme.metric(MetricRole::MENU_BORDER_WIDTH));

    for (const Slot& slot : slots) {
        MenuItem const& item = menus_[menu].items[slot.item];
        if ((item.flags & MenuItem::SEPARATOR) != 0) {
            int const y = slot.rect.y + height / 2;
            canvas.draw_hline(slot.rect.x + 8, slot.rect.x + slot.rect.width - 8, y,
                              theme.color(ColorRole::MENU_SEPARATOR));
            continue;
        }
        Color const text = (item.flags & MenuItem::DISABLED) != 0
                               ? theme.color(ColorRole::DISABLED_TEXT)
                               : theme.color(ColorRole::MENU_TEXT);
        canvas.draw_text({slot.rect.x + pad, slot.rect.y + (height - font->height()) / 2},
                         item.label, font, text);
        if ((item.flags & MenuItem::CHECKED) != 0) {
            int const cx = slot.rect.x + 8;
            int const cy = slot.rect.y + height / 2;
            canvas.draw_line({cx, cy}, {cx + 4, cy + 4}, text);
            canvas.draw_line({cx + 4, cy + 4}, {cx + 10, cy - 4}, text);
        }
    }
}

void Desktop::on_paint(Canvas& canvas, const PaintEvent&) {
    Theme& theme = Application::instance()->theme();
    canvas.fill_rect(rect_, theme.color(ColorRole::BACKGROUND));
    draw_bar(canvas);
    if (open_menu_ >= 0 && open_menu_ < static_cast<int>(menus_.size())) {
        draw_menu(canvas, open_menu_);
    }
}

void Desktop::on_mouse_down(const MouseEvent& event) {
    for (const Slot& slot : title_slots()) {
        if (slot.rect.contains(event.pos)) {
            bool const opening = open_menu_ != slot.menu;
            open_menu_ = opening ? slot.menu : -1;
            damage();
            if (opening && on_menu_opened) on_menu_opened(open_menu_);
            return;
        }
    }
    if (open_menu_ >= 0) {
        for (const Slot& slot : item_slots(open_menu_)) {
            if (!slot.rect.contains(event.pos)) continue;
            MenuItem const& item = menus_[open_menu_].items[slot.item];
            uint32_t const action = item.action_id;
            bool const acts = (item.flags & (MenuItem::DISABLED | MenuItem::SEPARATOR)) == 0;
            open_menu_ = -1;
            damage();
            if (acts && on_action) on_action(action);
            return;
        }
        open_menu_ = -1;
        damage();
    }
}

Size Desktop::preferred_size() const {
    return rect_.size();
}

} // namespace aegir::bureau
