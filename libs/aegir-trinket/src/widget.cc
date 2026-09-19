/*
 * Trinket Widget base class implementation.
 */

#include <aegir/trinket/widget.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/application.h>
#include <algorithm>

namespace aegir::trinket {

Widget::Widget() = default;

Widget::~Widget() = default;

void Widget::set_rect(Rect r) {
    if (rect_ != r) {
        rect_ = r;
        damage();
        if (parent_) {
            parent_->damage();
        }
    }
}

void Widget::set_focused(bool f) {
    if (focused_ != f) {
        focused_ = f;
        if (f) {
            on_focus_gained();
            dispatch_focus_gained();
        } else {
            on_focus_lost();
            dispatch_focus_lost();
        }
    }
}

void Widget::on_paint(Canvas& canvas, const PaintEvent&) {
    // Draw background if set
    Color bg = background_color();
    if (bg.a > 0) {
        canvas.fill_rect(rect_, bg);
    }
}

void Widget::on_mouse_down(const MouseEvent&) {}
void Widget::on_mouse_up(const MouseEvent&) {}
void Widget::on_mouse_move(const MouseEvent&) {}
void Widget::on_mouse_enter(const MouseEvent&) {}
void Widget::on_mouse_leave(const MouseEvent&) {}
void Widget::on_key_down(const KeyEvent&) {}
void Widget::on_key_up(const KeyEvent&) {}
void Widget::on_focus_gained() {}
void Widget::on_focus_lost() {}
void Widget::on_layout() {}

void Widget::damage(const Rect& r) {
    if (r.empty()) return;
    Rect damage_rect = r;
    if (damage_rect.width == 0 && damage_rect.height == 0) {
        damage_rect = rect_;
    }
    // Convert to parent coordinates
    if (parent_) {
        damage_rect.x += parent_->rect_.x;
        damage_rect.y += parent_->rect_.y;
        parent_->damage(damage_rect);
    } else if (Application::instance()) {
        // Top-level widget - post to application
        Application::instance()->post_event([damage_rect]() {
            // Handled by window's damage system
        });
    }
}

void Widget::dispatch_paint(Canvas& canvas, const PaintEvent& event) {
    // Clip to widget rect
    Rect clip = event.clip_rect.intersected(rect_);
    if (!clip.empty()) {
        canvas.set_clip_rect(clip);
        PaintEvent evt = event;
        evt.clip_rect = clip;
        on_paint(canvas, evt);
        canvas.clear_clip_rect();
    }
}

void Widget::dispatch_mouse_down(const MouseEvent& event) {
    if (!enabled_ || !visible_) return;
    if (rect_.contains(event.pos)) {
        on_mouse_down(event);
    }
}

void Widget::dispatch_mouse_up(const MouseEvent& event) {
    if (!enabled_ || !visible_) return;
    if (rect_.contains(event.pos)) {
        on_mouse_up(event);
    }
}

void Widget::dispatch_mouse_move(const MouseEvent& event) {
    if (!enabled_ || !visible_) return;
    if (rect_.contains(event.pos)) {
        on_mouse_move(event);
    }
}

void Widget::dispatch_mouse_enter(const MouseEvent& event) {
    if (!enabled_ || !visible_) return;
    on_mouse_enter(event);
}

void Widget::dispatch_mouse_leave(const MouseEvent& event) {
    if (!enabled_ || !visible_) return;
    on_mouse_leave(event);
}

void Widget::dispatch_key_down(const KeyEvent& event) {
    if (!enabled_ || !visible_) return;
    if (focused_) {
        on_key_down(event);
    }
}

void Widget::dispatch_key_up(const KeyEvent& event) {
    if (!enabled_ || !visible_) return;
    if (focused_) {
        on_key_up(event);
    }
}

void Widget::dispatch_focus_gained() {
    on_focus_gained();
}

void Widget::dispatch_focus_lost() {
    on_focus_lost();
}

void Widget::dispatch_layout() {
    on_layout();
    if (layout_) {
        layout_->layout(*static_cast<Container*>(this));
    }
}

// Container implementation
Container::Container() = default;
Container::~Container() = default;

void Container::add_child(std::unique_ptr<Widget> child) {
    if (child) {
        child->parent_ = this;
        children_.push_back(std::move(child));
    }
}

void Container::remove_child(Widget* child) {
    auto it = std::find_if(children_.begin(), children_.end(),
                           [child](const std::unique_ptr<Widget>& w) {
                               return w.get() == child;
                           });
    if (it != children_.end()) {
        (*it)->parent_ = nullptr;
        children_.erase(it);
    }
}

void Container::clear_children() {
    for (auto& child : children_) {
        child->parent_ = nullptr;
    }
    children_.clear();
}

std::vector<Widget*> Container::children_ptrs() {
    std::vector<Widget*> result;
    result.reserve(children_.size());
    for (auto& child : children_) {
        result.push_back(child.get());
    }
    return result;
}

void Container::set_layout(std::unique_ptr<Layout> layout) {
    layout_ = std::move(layout);
}

Widget* Container::child_at(Point p) const {
    // Check children in reverse order (topmost first)
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        Widget* child = it->get();
        if (child->visible() && child->rect().contains(p)) {
            return child;
        }
    }
    return nullptr;
}

void Container::on_paint(Canvas& canvas, const PaintEvent& event) {
    Widget::on_paint(canvas, event);
    // Paint children in order (bottom to top)
    for (const auto& child : children_) {
        if (child->visible()) {
            child->dispatch_paint(canvas, event);
        }
    }
}

void Container::on_layout() {
    Widget::on_layout();
    if (layout_) {
        layout_->layout(*this);
    }
}

} // namespace aegir::trinket