/*
 * The Workbench screen: the backdrop, the screen title bar, and its menus.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The bureau's content is one of these, its rectangle the whole screen
 * (specs/workbench.md). It paints the backdrop, the screen title bar across
 * the top, and, when a menu is open, that menu below the bar. It is a trinket
 * Widget rather than a bare canvas because its rectangle is the screen, so an
 * open menu is inside it and nothing clips it.
 *
 * The menu model is the toolkit's MenuBar's -- titles and items, an item's
 * action_id and flags -- so the bureau.menu server serializes the same shape
 * when it lands.
 */

#ifndef AEGIR_BUREAU_DESKTOP_H
#define AEGIR_BUREAU_DESKTOP_H

#include <aegir/trinket/menubar.h>
#include <aegir/trinket/widget.h>
#include <functional>
#include <string>
#include <vector>

namespace aegir::bureau {

class Desktop : public aegir::trinket::Widget {
public:
    using Menu = aegir::trinket::MenuBar::Menu;
    using MenuItem = aegir::trinket::MenuBar::MenuItem;

    Desktop();
    ~Desktop() override;

    void set_menus(std::vector<Menu> menus);

    // The action an activated item names, or its absence.
    std::function<void(uint32_t action_id)> on_action;
    // The menu a click opened, by index; the bureau logs its cue on it.
    std::function<void(int menu)> on_menu_opened;

protected:
    void on_paint(aegir::trinket::Canvas& canvas,
                  const aegir::trinket::PaintEvent& event) override;
    void on_mouse_down(const aegir::trinket::MouseEvent& event) override;
    aegir::trinket::Size preferred_size() const override;

private:
    /* A title's span in the bar, or an item's in an open menu. */
    struct Slot {
        aegir::trinket::Rect rect;
        int menu = 0;
        int item = 0;
    };

    int bar_height() const;
    int item_height() const;
    int menu_width(int menu) const;
    std::vector<Slot> title_slots() const;
    std::vector<Slot> item_slots(int menu) const;
    void draw_bar(aegir::trinket::Canvas& canvas);
    void draw_menu(aegir::trinket::Canvas& canvas, int menu);

    std::vector<Menu> menus_;
    int open_menu_ = -1;
};

} // namespace aegir::bureau

#endif // AEGIR_BUREAU_DESKTOP_H
