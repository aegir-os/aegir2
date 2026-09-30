/*
 * Host conformance for the toolkit's sizing contract and group layout
 * (specs/trinket/layout.md).
 *
 * A layout reads a child's minimum, preferred and maximum -- content-derived,
 * never the rectangle it holds -- and a GroupLayout shares the space above the
 * minima by weight, never past a maximum, on one axis. That arithmetic is pure:
 * synthetic blocks with a stated contract are enough to assert the group's own
 * sizes, the weighted water-fill with and without a cap, the cross-axis
 * alignments, a nested group, and that a second layout moves nothing.
 *
 * Host tools only (python3 and a C++ compiler); not part of any target build.
 * Run through scripts/check_layout.py.
 */

#include <aegir/trinket/layout.h>
#include <aegir/trinket/widget.h>
#include <aegir/trinket/window.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace aegir::trinket {
/* widget.cc's damage path names the window -- `window_->damage(r)` -- and the
 * check lays out but never draws. A window that swallows the damage is enough
 * (specs/trinket/layout.md). window.h is host-includable through the sel4 stub
 * beside this driver (scripts/layout_stub/sel4/sel4.h). */
void Window::damage(const Rect&) {}
}  // namespace aegir::trinket

namespace {

using namespace aegir::trinket;

unsigned g_checks = 0;
unsigned g_failures = 0;

void check(bool ok, std::string const& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

std::string rect_str(Rect const& r) {
    return "{" + std::to_string(r.x) + "," + std::to_string(r.y) + "," +
           std::to_string(r.width) + "," + std::to_string(r.height) + "}";
}

std::string size_str(Size const& s) {
    return std::to_string(s.width) + "x" + std::to_string(s.height);
}

/* A widget with a stated contract: minimum, preferred and maximum per axis. */
class Block : public Widget {
public:
    Block(Size minimum, Size preferred, Size maximum)
        : minimum_(minimum), preferred_(preferred), maximum_(maximum) {}

    Size minimum_size() const override { return minimum_; }
    Size preferred_size() const override { return preferred_; }
    Size maximum_size() const override { return maximum_; }

private:
    Size minimum_;
    Size preferred_;
    Size maximum_;
};

/* A container that can be asked to lay itself out; `on_layout` is protected. */
class Bench : public Container {
public:
    void arrange() { on_layout(); }
};

Block* add(Bench& bench, Size minimum, Size preferred, Size maximum = {}) {
    auto block = std::make_unique<Block>(minimum, preferred,
                                         maximum.width > 0 || maximum.height > 0
                                             ? maximum
                                             : preferred);
    Block* raw = block.get();
    bench.add_child(std::move(block));
    return raw;
}

void expect_rect(Widget* widget, Rect const& want, std::string const& what) {
    check(widget->rect() == want,
          what + ": got " + rect_str(widget->rect()) + ", want " + rect_str(want));
}

void expect_size(Size got, Size want, std::string const& what) {
    check(got == want, what + ": got " + size_str(got) + ", want " + size_str(want));
}

}  // namespace

int main() {
    /* 1. A vertical group of fixed blocks: no weight can grow what its maximum
     *    pins, so they keep their height and stretch across the group. */
    {
        Bench bench;
        auto layout = std::make_unique<GroupLayout>(GroupLayout::Orientation::VERTICAL, 4);
        GroupLayout* const group = layout.get();
        bench.set_layout(std::move(layout));
        Block* const a = add(bench, {10, 8}, {10, 8});
        Block* const b = add(bench, {10, 8}, {10, 8});
        Block* const c = add(bench, {10, 8}, {10, 8});
        bench.set_rect({0, 0, 30, 100});
        bench.arrange();

        expect_rect(a, {0, 0, 30, 8}, "fixed block 0");
        expect_rect(b, {0, 12, 30, 8}, "fixed block 1");
        expect_rect(c, {0, 24, 30, 8}, "fixed block 2");
        expect_size(group->minimum_size(bench), {10, 32}, "fixed group minimum");
        expect_size(group->preferred_size(bench), {10, 32}, "fixed group preferred");
        expect_size(group->maximum_size(bench), {10, 32}, "fixed group maximum");
    }

    /* 2. Weights: three growable blocks on 25/50/100 share the space above the
     *    minima; the truncation pixel goes to the first in child order. */
    {
        Bench bench;
        auto layout = std::make_unique<GroupLayout>(GroupLayout::Orientation::HORIZONTAL, 0);
        GroupLayout* const group = layout.get();
        bench.set_layout(std::move(layout));
        Block* const a = add(bench, {10, 10}, {10, 10}, {1000, 10});
        Block* const b = add(bench, {10, 10}, {10, 10}, {1000, 10});
        Block* const c = add(bench, {10, 10}, {10, 10}, {1000, 10});
        group->set_weight(a, 25);
        group->set_weight(b, 50);
        group->set_weight(c, 100);
        bench.set_rect({0, 0, 200, 10});
        bench.arrange();

        expect_rect(a, {0, 0, 35, 10}, "weighted block 25");
        expect_rect(b, {35, 0, 58, 10}, "weighted block 50");
        expect_rect(c, {93, 0, 107, 10}, "weighted block 100");
        expect_size(group->minimum_size(bench), {30, 10}, "weighted group minimum");
        expect_size(group->preferred_size(bench), {30, 10}, "weighted group preferred");
        expect_size(group->maximum_size(bench), {3000, 10}, "weighted group maximum");

        /* Idempotence: a second pass computes the same rectangles. */
        Rect const before_a = a->rect();
        Rect const before_b = b->rect();
        Rect const before_c = c->rect();
        bench.arrange();
        check(a->rect() == before_a && b->rect() == before_b && c->rect() == before_c,
              "a second layout moves nothing");
    }

    /* 3. A maximum caps the water-fill; the rest flows to the block that can
     *    still take it. */
    {
        Bench bench;
        auto layout = std::make_unique<GroupLayout>(GroupLayout::Orientation::HORIZONTAL, 0);
        bench.set_layout(std::move(layout));
        Block* const a = add(bench, {10, 10}, {10, 10}, {30, 10});
        Block* const b = add(bench, {10, 10}, {10, 10}, {1000, 10});
        bench.set_rect({0, 0, 100, 10});
        bench.arrange();

        expect_rect(a, {0, 0, 30, 10}, "capped block");
        expect_rect(b, {30, 0, 70, 10}, "uncapped block");
    }

    /* 4. A weight of 0 drops a growable block out of the distribution. */
    {
        Bench bench;
        auto layout = std::make_unique<GroupLayout>(GroupLayout::Orientation::HORIZONTAL, 0);
        GroupLayout* const group = layout.get();
        bench.set_layout(std::move(layout));
        Block* const a = add(bench, {10, 10}, {10, 10}, {1000, 10});
        Block* const b = add(bench, {10, 10}, {10, 10}, {1000, 10});
        group->set_weight(a, 0);
        bench.set_rect({0, 0, 100, 10});
        bench.arrange();

        expect_rect(a, {0, 0, 10, 10}, "weight-0 block");
        expect_rect(b, {10, 0, 90, 10}, "default-weight block");
    }

    /* 5. Cross-axis alignment: STRETCH fills, the others keep their preferred
     *    cross size against the group's cross length. */
    {
        Bench bench;
        auto layout = std::make_unique<GroupLayout>(GroupLayout::Orientation::VERTICAL, 0);
        GroupLayout* const group = layout.get();
        bench.set_layout(std::move(layout));
        Block* const a = add(bench, {10, 8}, {10, 8});
        Block* const b = add(bench, {10, 8}, {10, 8});
        Block* const c = add(bench, {10, 8}, {10, 8});
        group->set_align(a, GroupLayout::Align::CENTER);
        group->set_align(b, GroupLayout::Align::END);
        group->set_align(c, GroupLayout::Align::START);
        bench.set_rect({0, 0, 20, 60});
        bench.arrange();

        expect_rect(a, {5, 0, 10, 8}, "center-aligned block");
        expect_rect(b, {10, 8, 10, 8}, "end-aligned block");
        expect_rect(c, {0, 16, 10, 8}, "start-aligned block");
    }

    /* 6. A group in a group: the inner container reports its own layout's
     *    sizes, and the outer layout dispatches into it. */
    {
        Bench outer;
        auto outer_layout = std::make_unique<GroupLayout>(GroupLayout::Orientation::VERTICAL, 0);
        outer.set_layout(std::move(outer_layout));
        Block* const fixed = add(outer, {50, 20}, {50, 20});

        auto inner = std::make_unique<Bench>();
        auto inner_layout = std::make_unique<GroupLayout>(GroupLayout::Orientation::HORIZONTAL, 0);
        inner->set_layout(std::move(inner_layout));
        Block* const left = add(*inner, {10, 10}, {10, 10}, {1000, 10});
        Block* const right = add(*inner, {10, 10}, {10, 10}, {1000, 10});
        Bench* const inner_raw = inner.get();
        outer.add_child(std::move(inner));

        outer.set_rect({0, 0, 100, 100});
        outer.arrange();

        expect_rect(fixed, {0, 0, 100, 20}, "outer fixed block");
        expect_rect(inner_raw, {0, 20, 100, 10}, "nested group");
        expect_rect(left, {0, 20, 50, 10}, "nested left");
        expect_rect(right, {50, 20, 50, 10}, "nested right");
    }

    /* An empty group has no size. */
    {
        Bench bench;
        auto layout = std::make_unique<GroupLayout>(GroupLayout::Orientation::VERTICAL, 4);
        GroupLayout* const group = layout.get();
        bench.set_layout(std::move(layout));
        expect_size(group->preferred_size(bench), {0, 0}, "empty group");
    }

    std::printf("layout: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
