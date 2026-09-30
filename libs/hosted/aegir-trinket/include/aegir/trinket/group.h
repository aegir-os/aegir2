/*
 * Trinket Group - a MUI-style container.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A group arranges its children along one axis with a GroupLayout and draws an
 * optional frame from the theme (specs/trinket/layout.md). Weight and
 * cross-axis alignment live in the layout, so a child does not know its group,
 * and the frame's inset is the layout's too.
 */

#ifndef AEGIR_TRINKET_GROUP_H
#define AEGIR_TRINKET_GROUP_H

#include <aegir/trinket/layout.h>
#include <aegir/trinket/panel.h>
#include <aegir/trinket/unicode.h>
#include <aegir/trinket/widget.h>
#include <string>
#include <string_view>

namespace aegir::trinket {

class Canvas;

class Group : public Container {
public:
    using Orientation = GroupLayout::Orientation;
    using Align = GroupLayout::Align;
    /* The frame a group draws is a panel's (RAISED, SUNKEN, GROUP_BOX, ...);
     * FLAT is the default and draws nothing. */
    using Frame = Panel::Style;

    explicit Group(Orientation orientation = Orientation::VERTICAL,
                   int spacing = 0);
    ~Group() override;

    void set_orientation(Orientation o);
    Orientation orientation() const;
    void set_spacing(int s);
    int spacing() const;

    void set_frame(Frame frame);
    Frame frame() const { return frame_; }

    /* How far a frame's edge reaches into the group; a client sizing a window
     * around a framed group asks (the demo does, specs/trinket/layout.md). */
    static int frame_inset(Frame frame) noexcept;

    void set_title(std::u32string_view title);
    void set_title(std::string_view title);
    const std::u32string& title() const { return title_; }

    /* The per-child parameters, delegated to the group's layout. */
    void set_weight(Widget* child, int weight);
    void set_align(Widget* child, Align align);

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;

private:
    void update_inset();

    GroupLayout* group_ = nullptr;
    Frame frame_ = Frame::FLAT;
    std::u32string title_;
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_GROUP_H
