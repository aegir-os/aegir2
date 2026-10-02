/*
 * Host conformance for the tab group's strip layout (specs/trinket/tabs.md).
 *
 * The strip lays each tab out from its title's measured width, the horizontal
 * padding and the gap, and `tab_at` names the tab a point is over. It is pure
 * arithmetic -- no font, no theme -- so a host check pins it, including the
 * cases a screen shows only by luck: no pages, one page, and a point past the
 * last tab.
 *
 * Host tools only (python3 and a C++ compiler); not part of any target build.
 * Run through scripts/check_tab_group.py.
 */

#include <aegir/trinket/tab_group.h>
#include <aegir/trinket/widget.h>
#include <aegir/trinket/window.h>

#include <cstdio>
#include <string>
#include <vector>

namespace aegir::trinket {
/* widget.cc's damage path names the window, and tab_group.cc's metrics name the
 * application; the check maps arithmetic but never draws, so a window that
 * swallows the damage and a null application are enough
 * (specs/trinket/tabs.md). window.h is host-includable through the sel4 stub
 * beside the layout check (scripts/layout_stub/sel4/sel4.h). */
void Window::damage(const Rect&) {}
Application* Application::instance() { return nullptr; }
Font* Application::default_font() { return nullptr; }
}  // namespace aegir::trinket

namespace {

using aegir::trinket::TabGroup;

unsigned g_checks = 0;
unsigned g_failures = 0;

void check(bool ok, std::string const& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

void expect_layout(std::vector<int> const& titles, int padding, int gap,
                   std::vector<int> const& want_x, std::vector<int> const& want_width,
                   std::string const& what) {
    TabGroup::TabLayout const got = TabGroup::tab_layout(titles, padding, gap);
    check(got.x == want_x && got.width == want_width, what);
}

}  // namespace

int main() {
    /* No pages: no tabs. */
    expect_layout({}, 10, 2, {}, {}, "no pages, no tabs");

    /* Three tabs: each its title's width plus twice the padding, `gap` apart. */
    TabGroup::TabLayout const three = TabGroup::tab_layout({20, 30, 10}, 10, 2);
    check(three.x == std::vector<int>{0, 42, 94}, "three tabs, their x");
    check(three.width == std::vector<int>{40, 50, 30}, "three tabs, their width");

    /* `tab_at` names the tab a point is over, and -1 outside every tab. */
    check(TabGroup::tab_at(three, 0) == 0, "the first tab's left edge");
    check(TabGroup::tab_at(three, 39) == 0, "the first tab's last pixel");
    check(TabGroup::tab_at(three, 40) == -1, "the gap after the first is no tab");
    check(TabGroup::tab_at(three, 42) == 1, "the second tab's left edge");
    check(TabGroup::tab_at(three, 91) == 1, "the second tab's last pixel");
    check(TabGroup::tab_at(three, 92) == -1, "the gap after the second is no tab");
    check(TabGroup::tab_at(three, 94) == 2, "the third tab's left edge");
    check(TabGroup::tab_at(three, 123) == 2, "the third tab's last pixel");
    check(TabGroup::tab_at(three, 124) == -1, "past the last tab");
    check(TabGroup::tab_at(three, -1) == -1, "left of the first tab");

    /* A title narrower than its padding makes a zero-width tab, not a negative
     * one, and no point is ever over it. */
    TabGroup::TabLayout const tiny = TabGroup::tab_layout({0}, 0, 0);
    check(tiny.width == std::vector<int>{0}, "a bare title is a zero tab");
    check(TabGroup::tab_at(tiny, 0) == -1, "and nothing is over it");

    std::printf("tab-group: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
