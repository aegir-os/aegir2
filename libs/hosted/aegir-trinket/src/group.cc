/*
 * Trinket Group implementation (see the header for the shape).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/group.h>

#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/theme.h>

#include <memory>

namespace aegir::trinket {

/* How far a frame's edge reaches into the group: a bevel is one pixel, and a
 * group box leaves a little more so a child does not sit on the frame. */
int Group::frame_inset(Frame frame) noexcept {
    switch (frame) {
        case Frame::FLAT: return 0;
        case Frame::GROUP_BOX: return 4;
        case Frame::RAISED:
        case Frame::SUNKEN:
        case Frame::FRAME:
            return 2;
    }
    return 0;
}

Group::Group(Orientation orientation, int spacing) {
    auto layout = std::make_unique<GroupLayout>(orientation, spacing);
    group_ = layout.get();
    set_layout(std::move(layout));
    update_inset();
}

Group::~Group() = default;

void Group::set_orientation(Orientation o) {
    group_->set_orientation(o);
    damage();
}

Group::Orientation Group::orientation() const {
    return group_->orientation();
}

void Group::set_spacing(int s) {
    group_->set_spacing(s);
    damage();
}

int Group::spacing() const {
    return group_->spacing();
}

void Group::set_frame(Frame frame) {
    frame_ = frame;
    update_inset();
    damage();
}

void Group::set_title(std::u32string_view title) {
    title_ = std::u32string(title);
    /* The band the caption needs changes the inset, and the inset changes the
     * layout's preferred size (specs/trinket/layout.md). */
    update_inset();
    damage();
}

void Group::set_title(std::string_view title) {
    title_ = utf8_to_utf32(title);
    update_inset();
    damage();
}

void Group::set_weight(Widget* child, int weight) {
    group_->set_weight(child, weight);
    damage();
}

void Group::set_align(Widget* child, Align align) {
    group_->set_align(child, align);
    damage();
}

void Group::update_inset() {
    int const inset = frame_inset(frame_);
    /* The caption's band is the box's, not the frame's, so it goes on top of the
     * frame's own inset (specs/trinket/layout.md). */
    group_->set_inset(inset + title_band(), inset, inset, inset);
}

int Group::title_band() const noexcept {
    if (frame_ != Frame::GROUP_BOX || title_.empty()) {
        return 0;
    }
    Application* const app = Application::instance();
    Font* const font = app != nullptr ? app->default_font() : nullptr;
    return font != nullptr ? font->height() + 4 : 0;
}

void Group::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);
    if (frame_ != Frame::FLAT) {
        Application::instance()->theme().draw_panel(canvas, rect_, frame_, title_,
                                                    focused());
    }
    Container::on_paint(canvas, event);
}

}  // namespace aegir::trinket
