/*
 * Trinket Layout implementations.
 */

#include <aegir/trinket/layout.h>
#include <aegir/trinket/widget.h>
#include <algorithm>
#include <limits>

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
                /* The second pass is where a center child is placed, and it is
                 * the only place: setting it here too would hand it the
                 * zero rect this switch left `rect` as, and the next layout
                 * would change it back -- a rect change is a damage, a damage
                 * is a repaint, and a repaint lays out again, so the two
                 * passes would repaint each other forever. */
                continue;
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

// GroupLayout

GroupLayout::GroupLayout(Orientation orientation, int spacing)
    : orientation_(orientation), spacing_(spacing) {}

GroupLayout::Child* GroupLayout::find(Widget* child) {
    for (auto& c : children_) {
        if (c.widget == child) return &c;
    }
    return nullptr;
}

GroupLayout::Child const* GroupLayout::find(Widget* child) const {
    for (auto const& c : children_) {
        if (c.widget == child) return &c;
    }
    return nullptr;
}

void GroupLayout::set_weight(Widget* child, int weight) {
    Child* const c = find(child);
    if (c == nullptr) {
        children_.push_back({child, weight, Align::STRETCH});
    } else {
        c->weight = weight;
    }
}

int GroupLayout::weight(Widget* child) const {
    Child const* const c = find(child);
    return c != nullptr ? c->weight : kDefaultWeight;
}

void GroupLayout::set_align(Widget* child, Align align) {
    Child* const c = find(child);
    if (c == nullptr) {
        children_.push_back({child, kDefaultWeight, align});
    } else {
        c->align = align;
    }
}

GroupLayout::Align GroupLayout::align(Widget* child) const {
    Child const* const c = find(child);
    return c != nullptr ? c->align : Align::STRETCH;
}

namespace {

/* One visible child of a group: the sizes the contract reports and the
 * parameters the layout holds for it. */
struct GroupItem {
    Widget* widget = nullptr;
    Size min;
    Size pref;
    Size max;
    int weight = GroupLayout::kDefaultWeight;
    GroupLayout::Align align = GroupLayout::Align::STRETCH;
};

std::vector<GroupItem> group_items(Container const& container,
                                   GroupLayout const& layout) {
    std::vector<GroupItem> items;
    for (auto const& child : container.children()) {
        if (!child || !child->visible()) continue;
        GroupItem item;
        item.widget = child.get();
        item.min = child->minimum_size();
        item.pref = child->preferred_size();
        item.max = child->maximum_size();
        item.weight = layout.weight(item.widget);
        item.align = layout.align(item.widget);
        items.push_back(item);
    }
    return items;
}

/* Clamp a sum of sizes to int: a growable child may report a very large
 * maximum, and a group of them must not overflow (specs/trinket/layout.md). */
int add_sizes(int total, int part) {
    long long const sum = static_cast<long long>(total) + part;
    if (sum > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    return static_cast<int>(sum);
}

struct GroupSizes {
    Size minimum;
    Size preferred;
    Size maximum;
};

GroupSizes group_sizes(std::vector<GroupItem> const& items, bool horizontal,
                       int spacing) {
    int const gaps = items.empty() ? 0 : (static_cast<int>(items.size()) - 1) * spacing;
    int min_main = 0, pref_main = 0, max_main = 0;
    int min_cross = 0, pref_cross = 0, max_cross = 0;
    for (auto const& item : items) {
        int const min_m = horizontal ? item.min.width : item.min.height;
        int const pref_m = horizontal ? item.pref.width : item.pref.height;
        int const max_m = horizontal ? item.max.width : item.max.height;
        int const min_c = horizontal ? item.min.height : item.min.width;
        int const pref_c = horizontal ? item.pref.height : item.pref.width;
        int const max_c = horizontal ? item.max.height : item.max.width;
        min_main = add_sizes(min_main, min_m);
        pref_main = add_sizes(pref_main, pref_m);
        max_main = add_sizes(max_main, max_m);
        min_cross = std::max(min_cross, min_c);
        pref_cross = std::max(pref_cross, pref_c);
        max_cross = std::max(max_cross, max_c);
    }
    min_main = add_sizes(min_main, gaps);
    pref_main = add_sizes(pref_main, gaps);
    max_main = add_sizes(max_main, gaps);

    GroupSizes sizes;
    if (horizontal) {
        sizes.minimum = {min_main, min_cross};
        sizes.preferred = {pref_main, pref_cross};
        sizes.maximum = {max_main, max_cross};
    } else {
        sizes.minimum = {min_cross, min_main};
        sizes.preferred = {pref_cross, pref_main};
        sizes.maximum = {max_cross, max_main};
    }
    return sizes;
}

}  // namespace

Size GroupLayout::preferred_size(Container const& container) const {
    return group_sizes(group_items(container, *this),
                       orientation_ == Orientation::HORIZONTAL, spacing_).preferred;
}

Size GroupLayout::minimum_size(Container const& container) const {
    return group_sizes(group_items(container, *this),
                       orientation_ == Orientation::HORIZONTAL, spacing_).minimum;
}

Size GroupLayout::maximum_size(Container const& container) const {
    return group_sizes(group_items(container, *this),
                       orientation_ == Orientation::HORIZONTAL, spacing_).maximum;
}

void GroupLayout::layout(Container& container) {
    std::vector<GroupItem> const items = group_items(container, *this);
    if (items.empty()) return;

    bool const horizontal = orientation_ == Orientation::HORIZONTAL;
    Rect const bounds = container.rect();
    int const main_len = horizontal ? bounds.width : bounds.height;
    int const cross_len = horizontal ? bounds.height : bounds.width;
    int const main_origin = horizontal ? bounds.x : bounds.y;
    int const cross_origin = horizontal ? bounds.y : bounds.x;

    int const count = static_cast<int>(items.size());
    int const gaps = (count - 1) * spacing_;

    /* Each child starts at its minimum; the space above the minima is shared by
     * weight, never past a child's maximum. */
    std::vector<int> sizes(static_cast<size_t>(count));
    int sum_min = 0;
    for (int i = 0; i < count; ++i) {
        sizes[i] = horizontal ? items[i].min.width : items[i].min.height;
        sum_min = add_sizes(sum_min, sizes[i]);
    }

    int remaining = main_len - gaps - sum_min;
    while (remaining > 0) {
        int total_weight = 0;
        for (int i = 0; i < count; ++i) {
            int const max_main = horizontal ? items[i].max.width : items[i].max.height;
            if (items[i].weight > 0 && sizes[i] < max_main) {
                total_weight = add_sizes(total_weight, items[i].weight);
            }
        }
        if (total_weight == 0) break;

        int distributed = 0;
        for (int i = 0; i < count; ++i) {
            int const max_main = horizontal ? items[i].max.width : items[i].max.height;
            if (items[i].weight <= 0 || sizes[i] >= max_main) continue;
            long long const want =
                static_cast<long long>(remaining) * items[i].weight / total_weight;
            int const can_take = max_main - sizes[i];
            int const give = static_cast<int>(std::min<long long>(want, can_take));
            sizes[i] += give;
            distributed += give;
        }
        if (distributed == 0) break;
        remaining -= distributed;
    }

    /* Integer truncation can leave a few pixels; hand them out one at a time in
     * child order until they are used (specs/trinket/layout.md). */
    bool progress = true;
    while (remaining > 0 && progress) {
        progress = false;
        for (int i = 0; i < count && remaining > 0; ++i) {
            int const max_main = horizontal ? items[i].max.width : items[i].max.height;
            if (sizes[i] < max_main) {
                ++sizes[i];
                --remaining;
                progress = true;
            }
        }
    }

    int pos = main_origin;
    for (int i = 0; i < count; ++i) {
        int cross_size = cross_len;
        int cross_pos = cross_origin;
        if (items[i].align != Align::STRETCH) {
            int const pref_cross = horizontal ? items[i].pref.height : items[i].pref.width;
            cross_size = std::min(pref_cross, cross_len);
            if (items[i].align == Align::CENTER) {
                cross_pos = cross_origin + (cross_len - cross_size) / 2;
            } else if (items[i].align == Align::END) {
                cross_pos = cross_origin + cross_len - cross_size;
            }
        }
        Rect const rect = horizontal ? Rect{pos, cross_pos, sizes[i], cross_size}
                                     : Rect{cross_pos, pos, cross_size, sizes[i]};
        items[i].widget->set_rect(rect);
        pos += sizes[i] + spacing_;
    }
}

} // namespace aegir::trinket