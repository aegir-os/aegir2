/*
 * Host conformance for the slider's knob geometry (specs/trinket/slider.md).
 *
 * A slider is a mapping: a value in `[min, max]` to the knob's offset and length
 * along the track, and the value a drag to a track position means. It is pure
 * arithmetic, so a host check pins it -- including the cases a screen shows only
 * by luck: a degenerate range, a value past either end, and a track smaller than
 * the knob. The knob is a fixed length, which is the one thing a scrollbar's
 * thumb is not.
 *
 * Host tools only (python3 and a C++ compiler); not part of any target build.
 * Run through scripts/check_slider.py.
 */

#include <aegir/trinket/slider.h>
#include <aegir/trinket/widget.h>
#include <aegir/trinket/window.h>

#include <cstdio>
#include <string>

namespace aegir::trinket {
/* widget.cc's damage path names the window, and slider.cc's theme metrics name
 * the application; the check maps arithmetic but never draws, so a window that
 * swallows the damage and a null application are enough
 * (specs/trinket/slider.md). window.h is host-includable through the sel4 stub
 * beside the layout check (scripts/layout_stub/sel4/sel4.h). */
void Window::damage(const Rect&) {}
Application* Application::instance() { return nullptr; }
}  // namespace aegir::trinket

namespace {

using aegir::trinket::Slider;

unsigned g_checks = 0;
unsigned g_failures = 0;

void check(bool ok, std::string const& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

void expect_knob(int track, int min, int max, int value, int knob, int pos, int size,
                 std::string const& what) {
    Slider::Knob const k = Slider::knob_for(track, min, max, value, knob);
    check(k.pos == pos && k.size == size,
          what + ": got {pos " + std::to_string(k.pos) + ", size " + std::to_string(k.size) +
              "}, want {pos " + std::to_string(pos) + ", size " + std::to_string(size) + "}");
}

void expect_value(int track, int min, int max, int pos, int knob, int want,
                  std::string const& what) {
    int const v = Slider::value_for_pos(track, min, max, pos, knob);
    check(v == want, what + ": got " + std::to_string(v) + ", want " + std::to_string(want));
}

}  // namespace

int main() {
    /* The knob is its length; its offset is the value's share of the travel. */
    expect_knob(100, 0, 100, 0, 16, 0, 16, "the minimum sits at the start");
    expect_knob(100, 0, 100, 50, 16, 42, 16, "the middle sits mid-travel");
    expect_knob(100, 0, 100, 100, 16, 84, 16, "the maximum sits at the end");

    /* A range that does not start at zero maps the same. */
    expect_knob(100, 10, 20, 10, 16, 0, 16, "a shifted range starts at the start");
    expect_knob(100, 10, 20, 15, 16, 42, 16, "a shifted range's middle sits mid-travel");
    expect_knob(100, 10, 20, 20, 16, 84, 16, "a shifted range ends at the end");

    /* A value past either end clamps. */
    expect_knob(100, 0, 100, 999, 16, 84, 16, "a value past the end clamps");
    expect_knob(100, 0, 100, -5, 16, 0, 16, "a value before the start clamps");

    /* A degenerate range has nowhere to go: the knob sits at the start. */
    expect_knob(100, 5, 5, 5, 16, 0, 16, "a degenerate range sits at the start");

    /* A track smaller than the knob: the knob is the track. */
    expect_knob(10, 0, 100, 50, 16, 0, 10, "a tiny track holds a full knob");

    /* No track at all. */
    expect_knob(0, 0, 100, 50, 16, 0, 0, "no track, no knob");

    /* The inverse: a drag to a track offset. */
    expect_value(100, 0, 100, 0, 16, 0, "a drag to the start is the minimum");
    expect_value(100, 0, 100, 42, 16, 50, "a drag to the middle is the middle value");
    expect_value(100, 0, 100, 84, 16, 100, "a drag to the end is the maximum");
    expect_value(100, 0, 100, 999, 16, 100, "a drag past the end clamps");
    expect_value(100, 0, 100, -5, 16, 0, "a drag before the start clamps");
    expect_value(100, 10, 20, 42, 16, 15, "a shifted range's middle is its middle value");
    expect_value(100, 5, 5, 50, 16, 5, "a degenerate range has one value");

    std::printf("slider: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
