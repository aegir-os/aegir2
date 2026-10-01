/*
 * Host conformance for the list's scroll offset (specs/trinket/listview.md).
 *
 * A list is a window onto rows: `first` is the top visible row, and scrolling
 * clamps it to `[0, count - visible]`. It is pure arithmetic, so a host check
 * pins it -- including the cases a screen shows only by luck: an empty list, a
 * list shorter than the view, and a count that shrank under the offset.
 *
 * Host tools only (python3 and a C++ compiler); not part of any target build.
 * Run through scripts/check_listview.py.
 */

#include <aegir/trinket/listview.h>
#include <aegir/trinket/widget.h>
#include <aegir/trinket/window.h>

#include <cstdio>
#include <string>

namespace aegir::trinket {
/* widget.cc's damage path names the window, and listview.cc's metrics name the
 * application; the check maps arithmetic but never draws, so a window that
 * swallows the damage and a null application are enough
 * (specs/trinket/listview.md). window.h is host-includable through the sel4 stub
 * beside the layout check (scripts/layout_stub/sel4/sel4.h). */
void Window::damage(const Rect&) {}
Application* Application::instance() { return nullptr; }
Font* Application::default_font() { return nullptr; }
}  // namespace aegir::trinket

namespace {

using aegir::trinket::ListView;

unsigned g_checks = 0;
unsigned g_failures = 0;

void check(bool ok, std::string const& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

void expect(int value, int count, int visible, int want, std::string const& what) {
    int const got = ListView::clamp_first(value, count, visible);
    check(got == want, what + ": got " + std::to_string(got) + ", want " + std::to_string(want));
}

}  // namespace

int main() {
    /* The offset moves within the range. */
    expect(0, 100, 10, 0, "the top of a long list stays at the top");
    expect(5, 100, 10, 5, "a scroll inside the range is kept");
    expect(90, 100, 10, 90, "the last window sits at the bottom");

    /* And clamps at either end. */
    expect(-3, 100, 10, 0, "a scroll before the top clamps");
    expect(999, 100, 10, 90, "a scroll past the bottom clamps");

    /* A list that fits does not scroll at all. */
    expect(0, 5, 10, 0, "a list shorter than the view stays at the top");
    expect(4, 5, 10, 0, "even a scroll in a short list clamps to the top");
    expect(0, 10, 10, 0, "a list exactly the view's size does not scroll");

    /* An empty list, and no visible rows at all. */
    expect(3, 0, 10, 0, "an empty list has no offset");
    expect(3, 100, 0, 3, "no visible rows leaves the offset where it is");

    /* A count that shrank under the offset clamps to the new end. */
    expect(50, 20, 10, 10, "a count that shrank under the offset clamps");

    std::printf("listview: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
