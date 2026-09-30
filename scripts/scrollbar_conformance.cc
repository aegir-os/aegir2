/*
 * Host conformance for the scrollbar's thumb geometry (specs/trinket/scrollbar.md).
 *
 * A scrollbar is a mapping: content of `total` units with `page` visible, the
 * thumb's length and offset along the track, and the value a drag to a track
 * position means. It is pure arithmetic, so a host check pins it -- including
 * the cases that a screen would show only by luck: nothing to scroll, a page
 * larger than the track's share, and a drag past either end.
 *
 * Host tools only (python3 and a C++ compiler); not part of any target build.
 * Run through scripts/check_scrollbar.py.
 */

#include <aegir/trinket/scrollbar.h>
#include <aegir/trinket/widget.h>
#include <aegir/trinket/window.h>

#include <cstdio>
#include <string>

namespace aegir::trinket {
/* widget.cc's damage path names the window, and scrollbar.cc's theme metrics
 * name the application; the check maps arithmetic but never draws, so a window
 * that swallows the damage and a null application are enough
 * (specs/trinket/scrollbar.md). window.h is host-includable through the sel4
 * stub beside the layout check (scripts/layout_stub/sel4/sel4.h). */
void Window::damage(const Rect&) {}
Application* Application::instance() { return nullptr; }
}  // namespace aegir::trinket

namespace {

using aegir::trinket::Scrollbar;

unsigned g_checks = 0;
unsigned g_failures = 0;

void check(bool ok, std::string const& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

void expect_thumb(int track, int total, int page, int value, int min_handle,
                  int pos, int size, std::string const& what) {
    Scrollbar::Thumb const t = Scrollbar::thumb_for(track, total, page, value, min_handle);
    check(t.pos == pos && t.size == size,
          what + ": got {pos " + std::to_string(t.pos) + ", size " + std::to_string(t.size) +
              "}, want {pos " + std::to_string(pos) + ", size " + std::to_string(size) + "}");
}

void expect_value(int track, int total, int page, int pos, int min_handle, int want,
                  std::string const& what) {
    int const v = Scrollbar::value_for_pos(track, total, page, pos, min_handle);
    check(v == want, what + ": got " + std::to_string(v) + ", want " + std::to_string(want));
}

}  // namespace

int main() {
    /* Nothing to scroll: the thumb fills the track. */
    expect_thumb(100, 100, 100, 0, 30, 0, 100, "a full page fills the track");
    expect_thumb(100, 40, 100, 0, 30, 0, 100, "a page larger than the content");

    /* The thumb is the page's share, but never below the minimum. */
    expect_thumb(100, 200, 50, 0, 30, 0, 30, "a small page keeps the minimum handle");
    expect_thumb(100, 200, 100, 0, 30, 0, 50, "a half page is half the track");

    /* Its offset is the value's share of the travel. */
    expect_thumb(100, 200, 50, 0, 30, 0, 30, "the top value sits at the start");
    expect_thumb(100, 200, 50, 75, 30, 35, 30, "the middle value sits mid-travel");
    expect_thumb(100, 200, 50, 150, 30, 70, 30, "the last value sits at the end");

    /* A value past the end clamps to the end. */
    expect_thumb(100, 200, 50, 999, 30, 70, 30, "a value past the end clamps");

    /* A track smaller than the minimum handle: the handle is the track. */
    expect_thumb(20, 200, 50, 0, 30, 0, 20, "a tiny track holds a full handle");

    /* No track at all. */
    expect_thumb(0, 200, 50, 0, 30, 0, 0, "no track, no thumb");

    /* The inverse: a drag to a track offset. */
    expect_value(100, 200, 50, 0, 30, 0, "a drag to the start is the top value");
    expect_value(100, 200, 50, 35, 30, 75, "a drag to the middle is the middle value");
    expect_value(100, 200, 50, 70, 30, 150, "a drag to the end is the last value");
    expect_value(100, 200, 50, 999, 30, 150, "a drag past the end clamps");
    expect_value(100, 200, 50, -5, 30, 0, "a drag before the start clamps");
    expect_value(100, 100, 100, 50, 30, 0, "nothing to scroll has no value");

    std::printf("scrollbar: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
