/*
 * Trinket Cycle - a named choice clicked through (specs/trinket/cycle.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_CYCLE_H
#define AEGIR_TRINKET_CYCLE_H

#include <aegir/trinket/widget.h>
#include <functional>
#include <string>
#include <vector>

namespace aegir::trinket {

class Canvas;

/* A named choice you click through: a click advances to the next entry, a click
 * on the left arrow cell -- or a Shift+click -- goes back, Left/Right do the
 * same when focused and Home/End jump to the ends. The active entry's text is
 * centred between the two arrow marks, which are the imported
 * `ArrowLeft`/`ArrowRight` art. MUI's cycle also pops its entries up when the
 * text is hit; that needs the list a popup opens and waits (specs/trinket/cycle.md). */
class Cycle : public Widget {
public:
    Cycle() = default;
    explicit Cycle(std::vector<std::u32string> entries);
    ~Cycle() override;

    void set_entries(std::vector<std::u32string> entries);
    void add(std::u32string_view entry);
    int count() const { return static_cast<int>(entries_.size()); }
    const std::u32string& entry(int index) const;

    void set_active(int index);  // programmatic: no on_changed
    int active() const { return active_; }
    const std::u32string& active_entry() const;

    /* Whether the last entry's next is the first. A chain that is not a cycle
     * clamps at the ends instead. */
    void set_wrap(bool wrap) { wrap_ = wrap; }
    bool wrap() const { return wrap_; }

    /* The user cycled (a click, an arrow or Home/End), not set_active. */
    std::function<void(int)> on_changed;

    /* The index `delta` steps from `active` in a list of `count`, wrapping or
     * clamping. Pure, so the host check pins it (specs/trinket/cycle.md). */
    static int step_index(int active, int count, int delta, bool wrap);

    bool focusable() const override { return true; }
    Size preferred_size() const override;
    Size minimum_size() const override;
    Size maximum_size() const override;

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    void on_mouse_down(const MouseEvent& event) override;
    void on_mouse_up(const MouseEvent& event) override;
    void on_mouse_enter(const MouseEvent& event) override;
    void on_mouse_leave(const MouseEvent& event) override;
    void on_key_down(const KeyEvent& event) override;

private:
    /* The divider's line and the mark's square: the widget owns them so the
     * text is centred against the same edges the theme draws. */
    Rect divider_rect() const;
    Rect mark_rect() const;
    void step(int delta);   // user-driven
    void go_to(int index);  // user-driven

    std::vector<std::u32string> entries_;
    int active_ = 0;
    bool wrap_ = true;
    bool pressed_ = false;
    bool hovered_ = false;
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_CYCLE_H
