/*
 * Trinket Layout implementations.
 */

#include <aegir/trinket/layout.h>
#include <aegir/trinket/widget.h>
#include <algorithm>

namespace aegir::trinket {

// FlowLayout
FlowLayout::FlowLayout(Direction dir, int spacing, Alignment align)
    : direction_(dir), spacing_(spacing), alignment_(align) {}

void FlowLayout::layout(Container& container) {
    const auto& children = container.children();
    if (children.empty()) return;

    Rect bounds = container.rect();
    int x = bounds.x;
    int y = bounds.y;
    int line_start = direction_ == Direction::HORIZONTAL ? x : y;
    int line_size = 0;  // max height (horizontal) or width (vertical) in current line
    int line_pos = line_start;
    int main_pos = direction_ == Direction::HORIZONTAL ? y : x;

    std::vector<std::pair<Widget*, Rect>> placed;

    for (const auto& child_ptr : children) {
        Widget* child = child_ptr.get();
        if (!child->visible()) continue;

        Size pref = child->preferred_size();
        int main_size = direction_ == Direction::HORIZONTAL ? pref.height : pref.width;
        int cross_size = direction_ == Direction::HORIZONTAL ? pref.width : pref.height;

        // Check if we need to wrap
        if (wrap_ && line_pos + cross_size > (direction_ == Direction::HORIZONTAL ? bounds.width : bounds.height)) {
            // Move to next line
            main_pos += line_size + spacing_;
            line_pos = line_start;
            line_size = 0;
        }

        int cross_pos = line_pos;
        if (alignment_ == Alignment::CENTER) {
            cross_pos += (direction_ == Direction::HORIZONTAL ?
                          (bounds.width - line_size) / 2 :
                          (bounds.height - line_size) / 2);
        } else if (alignment_ == Alignment::END) {
            cross_pos += direction_ == Direction::HORIZONTAL ?
                         (bounds.width - line_size) :
                         (bounds.height - line_size);
        } else if (alignment_ == Alignment::STRETCH) {
            cross_size = direction_ == Direction::HORIZONTAL ? bounds.width : bounds.height;
        }

        Rect child_rect;
        if (direction_ == Direction::HORIZONTAL) {
            child_rect = {cross_pos, main_pos, cross_size, main_size};
        } else {
            child_rect = {main_pos, cross_pos, main_size, cross_size};
        }

        child->set_rect(child_rect);
        placed.emplace_back(child, child_rect);

        line_pos += cross_size + spacing_;
        line_size = std::max(line_size, main_size);
    }

    // Apply stretch alignment if needed
    if (alignment_ == Alignment::STRETCH) {
        for (auto& [child, rect] : placed) {
            if (direction_ == Direction::HORIZONTAL) {
                rect.width = bounds.width;
            } else {
                rect.height = bounds.height;
            }
            child->set_rect(rect);
        }
    }
}

Size FlowLayout::preferred_size(const Container& container) const {
    const auto& children = container.children();
    if (children.empty()) return {0, 0};

    int total_main = 0;
    int total_cross = 0;
    int line_main = 0;
    int line_cross = 0;

    for (const auto& child_ptr : children) {
        Widget* child = child_ptr.get();
        if (!child->visible()) continue;

        Size pref = child->preferred_size();
        int main = direction_ == Direction::HORIZONTAL ? pref.height : pref.width;
        int cross = direction_ == Direction::HORIZONTAL ? pref.width : pref.height;

        if (wrap_ && line_cross + cross > 10000) {  // Approximate - real layout uses actual bounds
            total_main += line_main + spacing_;
            total_cross = std::max(total_cross, line_cross);
            line_main = 0;
            line_cross = 0;
        }

        line_main = std::max(line_main, main);
        line_cross += cross + spacing_;
    }

    total_main += line_main;
    total_cross = std::max(total_cross, line_cross);

    if (direction_ == Direction::HORIZONTAL) {
        return {total_cross, total_main};
    } else {
        return {total_main, total_cross};
    }
}

// GridLayout
GridLayout::GridLayout(int columns, int spacing) : columns_(std::max(1, columns)), spacing_(spacing) {}

GridLayout::~GridLayout() = default;

void GridLayout::set_row_stretch(int row, int stretch) {
    if (row >= 0) {
        if (row >= static_cast<int>(row_stretch_.size())) row_stretch_.resize(row + 1, 0);
        row_stretch_[row] = stretch;
    }
}

void GridLayout::set_column_stretch(int col, int stretch) {
    if (col >= 0) {
        if (col >= static_cast<int>(col_stretch_.size())) col_stretch_.resize(col + 1, 0);
        col_stretch_[col] = stretch;
    }
}

void GridLayout::layout(Container& container) {
    const auto& children = container.children();
    if (children.empty()) return;

    Rect bounds = container.rect();
    int cols = std::min(columns_, static_cast<int>(children.size()));
    int rows = (children.size() + cols - 1) / cols;

    // Calculate column widths and row heights
    std::vector<int> col_widths(cols, 0);
    std::vector<int> row_heights(rows, 0);

    int idx = 0;
    for (const auto& child_ptr : children) {
        Widget* child = child_ptr.get();
        if (!child->visible()) continue;

        int col = idx % cols;
        int row = idx / cols;
        Size pref = child->preferred_size();
        col_widths[col] = std::max(col_widths[col], pref.width);
        row_heights[row] = std::max(row_heights[row], pref.height);
        idx++;
    }

    // Apply stretch
    int remaining_width = bounds.width - (cols - 1) * spacing_;
    for (int c = 0; c < cols; ++c) {
        remaining_width -= col_widths[c];
    }
    if (remaining_width > 0) {
        int total_stretch = 0;
        for (int c = 0; c < cols; ++c) {
            total_stretch += (c < static_cast<int>(col_stretch_.size()) ? col_stretch_[c] : 0);
        }
        if (total_stretch > 0) {
            for (int c = 0; c < cols; ++c) {
                int stretch = (c < static_cast<int>(col_stretch_.size()) ? col_stretch_[c] : 0);
                col_widths[c] += (remaining_width * stretch) / total_stretch;
            }
        }
    }

    int remaining_height = bounds.height - (rows - 1) * spacing_;
    for (int r = 0; r < rows; ++r) {
        remaining_height -= row_heights[r];
    }
    if (remaining_height > 0) {
        int total_stretch = 0;
        for (int r = 0; r < rows; ++r) {
            total_stretch += (r < static_cast<int>(row_stretch_.size()) ? row_stretch_[r] : 0);
        }
        if (total_stretch > 0) {
            for (int r = 0; r < rows; ++r) {
                int stretch = (r < static_cast<int>(row_stretch_.size()) ? row_stretch_[r] : 0);
                row_heights[r] += (remaining_height * stretch) / total_stretch;
            }
        }
    }

    // Position children
    idx = 0;
    for (const auto& child_ptr : children) {
        Widget* child = child_ptr.get();
        if (!child->visible()) { idx++; continue; }

        int col = idx % cols;
        int row = idx / cols;

        int x = bounds.x;
        for (int c = 0; c < col; ++c) x += col_widths[c] + spacing_;
        int y = bounds.y;
        for (int r = 0; r < row; ++r) y += row_heights[r] + spacing_;

        Rect rect = {x, y, col_widths[col], row_heights[row]};
        child->set_rect(rect);
        idx++;
    }
}

Size GridLayout::preferred_size(const Container& container) const {
    const auto& children = container.children();
    if (children.empty()) return {0, 0};

    int cols = std::min(columns_, static_cast<int>(children.size()));
    int rows = (children.size() + cols - 1) / cols;

    std::vector<int> col_widths(cols, 0);
    std::vector<int> row_heights(rows, 0);

    int idx = 0;
    for (const auto& child_ptr : children) {
        Widget* child = child_ptr.get();
        if (!child->visible()) { idx++; continue; }

        int col = idx % cols;
        int row = idx / cols;
        Size pref = child->preferred_size();
        col_widths[col] = std::max(col_widths[col], pref.width);
        row_heights[row] = std::max(row_heights[row], pref.height);
        idx++;
    }

    int width = (cols - 1) * spacing_;
    int height = (rows - 1) * spacing_;
    for (int c = 0; c < cols; ++c) width += col_widths[c];
    for (int r = 0; r < rows; ++r) height += row_heights[r];

    return {width, height};
}

// BorderLayout
BorderLayout::BorderLayout(int spacing) : spacing_(spacing) {}

void BorderLayout::add_widget(Widget* widget, Region region) {
    items_.push_back({widget, region});
}

void BorderLayout::layout(Container& container) {
    Rect bounds = container.rect();
    Rect remaining = bounds;

    // First pass: NORTH, SOUTH, EAST, WEST
    for (const auto& item : items_) {
        Widget* widget = item.widget;
        if (!widget || !widget->visible()) continue;

        Size pref = widget->preferred_size();
        Rect rect;

        switch (item.region) {
            case Region::NORTH:
                rect = {remaining.x, remaining.y, remaining.width, pref.height};
                remaining.y += pref.height + spacing_;
                remaining.height -= pref.height + spacing_;
                break;
            case Region::SOUTH:
                rect = {remaining.x, remaining.y + remaining.height - pref.height,
                        remaining.width, pref.height};
                remaining.height -= pref.height + spacing_;
                break;
            case Region::WEST:
                rect = {remaining.x, remaining.y, pref.width, remaining.height};
                remaining.x += pref.width + spacing_;
                remaining.width -= pref.width + spacing_;
                break;
            case Region::EAST:
                rect = {remaining.x + remaining.width - pref.width, remaining.y,
                        pref.width, remaining.height};
                remaining.width -= pref.width + spacing_;
                break;
            case Region::CENTER:
                break;  // Handle in second pass
        }
        widget->set_rect(rect);
    }

    // Second pass: CENTER gets remaining space
    for (const auto& item : items_) {
        if (item.region == Region::CENTER && item.widget && item.widget->visible()) {
            item.widget->set_rect(remaining);
        }
    }
}

Size BorderLayout::preferred_size(const Container& container) const {
    static_cast<void>(container);  // the border regions size to their widgets
    int width = 0, height = 0;
    int center_w = 0, center_h = 0;

    for (const auto& item : items_) {
        if (!item.widget || !item.widget->visible()) continue;
        Size pref = item.widget->preferred_size();

        switch (item.region) {
            case Region::NORTH:
            case Region::SOUTH:
                width = std::max(width, pref.width);
                height += pref.height + spacing_;
                break;
            case Region::WEST:
            case Region::EAST:
                height = std::max(height, pref.height);
                width += pref.width + spacing_;
                break;
            case Region::CENTER:
                center_w = std::max(center_w, pref.width);
                center_h = std::max(center_h, pref.height);
                break;
        }
    }

    width = std::max(width, center_w);
    height = std::max(height, center_h);
    return {width, height};
}

// AnchorLayout
void AnchorLayout::set_anchor(Widget* widget, Anchor anchor) {
    for (auto& item : items_) {
        if (item.widget == widget) {
            item.anchor = anchor;
            return;
        }
    }
    items_.push_back({widget, anchor, {}});
}

void AnchorLayout::set_margins(Widget* widget, Rect margins) {
    for (auto& item : items_) {
        if (item.widget == widget) {
            item.margins = margins;
            return;
        }
    }
    items_.push_back({widget, Anchor::NONE, margins});
}

void AnchorLayout::layout(Container& container) {
    Rect bounds = container.rect();

    for (const auto& item : items_) {
        Widget* widget = item.widget;
        if (!widget || !widget->visible()) continue;

        Size pref = widget->preferred_size();
        Rect rect;
        Anchor a = item.anchor;
        Rect margins = item.margins;

        // Default position: center
        int x = bounds.x + (bounds.width - pref.width) / 2;
        int y = bounds.y + (bounds.height - pref.height) / 2;

        if (a & Anchor::LEFT) x = bounds.x + margins.x;
        if (a & Anchor::RIGHT) x = bounds.x + bounds.width - pref.width - margins.width;
        if (a & Anchor::HCENTER) x = bounds.x + (bounds.width - pref.width) / 2;

        if (a & Anchor::TOP) y = bounds.y + margins.y;
        if (a & Anchor::BOTTOM) y = bounds.y + bounds.height - pref.height - margins.height;
        if (a & Anchor::VCENTER) y = bounds.y + (bounds.height - pref.height) / 2;

        rect = {x, y, pref.width, pref.height};
        widget->set_rect(rect);
    }
}

Size AnchorLayout::preferred_size(const Container& container) const {
    static_cast<void>(container);  // anchored widgets keep their own sizes
    Size max_size;
    for (const auto& item : items_) {
        if (!item.widget || !item.widget->visible()) continue;
        Size pref = item.widget->preferred_size();
        max_size.width = std::max(max_size.width, pref.width);
        max_size.height = std::max(max_size.height, pref.height);
    }
    return max_size;
}

} // namespace aegir::trinket