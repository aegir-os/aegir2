/*
 * Trinket Layout system.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_LAYOUT_H
#define AEGIR_TRINKET_LAYOUT_H

#include <aegir/trinket/point.h>
#include <memory>
#include <vector>

namespace aegir::trinket {

class Container;
class Widget;

class Layout {
public:
    virtual ~Layout() = default;

    // Called when container needs to arrange children
    virtual void layout(Container& container) = 0;

    // Return preferred size for container
    virtual Size preferred_size(const Container& container) const = 0;

    // Minimum size constraint
    virtual Size minimum_size(const Container& container) const { return preferred_size(container); }
};

class FlowLayout : public Layout {
public:
    enum class Direction { HORIZONTAL, VERTICAL };
    enum class Alignment { START, CENTER, END, STRETCH };

    FlowLayout(Direction dir = Direction::HORIZONTAL, int spacing = 8,
               Alignment align = Alignment::START);

    void set_direction(Direction d) { direction_ = d; }
    void set_spacing(int s) { spacing_ = s; }
    void set_alignment(Alignment a) { alignment_ = a; }
    void set_wrap(bool w) { wrap_ = w; }

    void layout(Container& container) override;
    Size preferred_size(const Container& container) const override;

private:
    Direction direction_;
    int spacing_;
    Alignment alignment_;
    bool wrap_ = true;
};

class GridLayout : public Layout {
public:
    GridLayout(int columns = 2, int spacing = 8);
    ~GridLayout() override;

    void set_columns(int c) { columns_ = std::max(1, c); }
    void set_spacing(int s) { spacing_ = s; }
    void set_row_stretch(int row, int stretch);
    void set_column_stretch(int col, int stretch);

    void layout(Container& container) override;
    Size preferred_size(const Container& container) const override;

private:
    int columns_;
    int spacing_;
    std::vector<int> row_stretch_;
    std::vector<int> col_stretch_;
};

class BorderLayout : public Layout {
public:
    enum class Region { NORTH, SOUTH, EAST, WEST, CENTER };

    BorderLayout(int spacing = 0);

    void add_widget(Widget* widget, Region region);
    void set_spacing(int s) { spacing_ = s; }

    void layout(Container& container) override;
    Size preferred_size(const Container& container) const override;

private:
    struct Item { Widget* widget = nullptr; Region region = Region::CENTER; };
    std::vector<Item> items_;
    int spacing_ = 0;
};

class AnchorLayout : public Layout {
public:
    enum class Anchor { NONE = 0,
        LEFT = 1, RIGHT = 2, HCENTER = 4,
        TOP = 8, BOTTOM = 16, VCENTER = 32 };

    AnchorLayout() = default;

    void set_anchor(Widget* widget, Anchor anchor);
    void set_margins(Widget* widget, Rect margins);

    void layout(Container& container) override;
    Size preferred_size(const Container& container) const override;

private:
    struct Item { Widget* widget = nullptr; Anchor anchor = Anchor::NONE; Rect margins; };
    std::vector<Item> items_;
};

/* `Anchor` is a bit set, so the tests in the layout are bitwise. A scoped enum
 * has no operators of its own; `&` answers "is this bit set" and `|` combines
 * anchors. */
constexpr bool operator&(AnchorLayout::Anchor left, AnchorLayout::Anchor right) noexcept
{
    return (static_cast<int>(left) & static_cast<int>(right)) != 0;
}

constexpr AnchorLayout::Anchor operator|(AnchorLayout::Anchor left,
                                         AnchorLayout::Anchor right) noexcept
{
    return static_cast<AnchorLayout::Anchor>(static_cast<int>(left) | static_cast<int>(right));
}

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_LAYOUT_H