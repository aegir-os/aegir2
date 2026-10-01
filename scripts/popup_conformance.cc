/*
 * Host conformance for the popup anchor rule (specs/trinket/popup.md).
 *
 * A popup is placed below the widget that opened it, above it when it does not
 * fit below, and inside the bounds either way. It is pure arithmetic, so a host
 * check pins it -- including the cases a screen shows only by luck: a popup that
 * must flip above, one taller or wider than the bounds, and one that would run
 * off the right edge.
 *
 * Host tools only (python3 and a C++ compiler); not part of any target build.
 * Run through scripts/check_popup.py.
 */

#include <aegir/trinket/popup.h>

#include <cstdio>
#include <string>

namespace {

using aegir::trinket::Rect;
using aegir::trinket::Size;
using aegir::trinket::popup_rect;

unsigned g_checks = 0;
unsigned g_failures = 0;

void check(bool ok, std::string const& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

void expect(Rect anchor, Size popup, Rect bounds, Rect want, std::string const& what) {
    Rect const got = popup_rect(anchor, popup, bounds);
    bool const same = got.x == want.x && got.y == want.y && got.width == want.width &&
                      got.height == want.height;
    check(same, what + ": got {" + std::to_string(got.x) + "," + std::to_string(got.y) + " " +
                    std::to_string(got.width) + "x" + std::to_string(got.height) + "}, want {" +
                    std::to_string(want.x) + "," + std::to_string(want.y) + " " +
                    std::to_string(want.width) + "x" + std::to_string(want.height) + "}");
}

}  // namespace

int main() {
    Rect const window{0, 0, 200, 200};
    Size const small{60, 40};

    /* A popup that fits below sits below, at the anchor's x. */
    expect({10, 10, 100, 20}, small, window, {10, 30, 60, 40}, "a popup that fits sits below");

    /* One that does not fit below goes above. */
    expect({10, 150, 100, 20}, small, window, {10, 110, 60, 40},
           "a popup that does not fit below goes above");

    /* Exactly touching the foot still fits below. */
    expect({10, 140, 100, 20}, small, window, {10, 160, 60, 40},
           "a popup ending at the foot still sits below");

    /* Taller than the bounds: it is the bounds' height, at the foot. */
    expect({0, 10, 100, 20}, {60, 500}, window, {0, 0, 60, 200},
           "a popup taller than the bounds is the bounds");

    /* Wider than the bounds: it is the bounds' width, at the anchor's x. */
    expect({0, 10, 100, 20}, {500, 40}, window, {0, 30, 200, 40},
           "a popup wider than the bounds is the bounds");

    /* Running past the right edge pulls it left. */
    expect({150, 10, 40, 20}, {100, 40}, window, {100, 30, 100, 40},
           "a popup past the right edge is pulled left");

    /* An anchor at the right with a popup wider than the bounds: the bounds. */
    expect({150, 10, 40, 20}, {300, 40}, window, {0, 30, 200, 40},
           "a popup wider than the bounds starts at the left");

    /* A bounds with a non-zero origin: the clamps are to the bounds, not zero. */
    Rect const offset{40, 50, 100, 100};
    expect({40, 50, 100, 10}, small, offset, {40, 60, 60, 40},
           "a popup below an anchor at the bounds' top");
    expect({40, 140, 100, 10}, small, offset, {40, 100, 60, 40},
           "a popup above an anchor at the bounds' foot");

    std::printf("popup: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
