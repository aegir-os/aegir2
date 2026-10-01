/*
 * Host conformance for the cycle's stepping (specs/trinket/cycle.md).
 *
 * A cycle is a mapping: an active index in a list of `count`, stepped by a
 * delta, wrapping at the ends or clamping. It is pure arithmetic, so a host
 * check pins it -- including the cases a click reaches only by getting there: a
 * wrap at either end, a chain told not to wrap, a one-entry list, and an empty
 * one.
 *
 * Host tools only (python3 and a C++ compiler); not part of any target build.
 * Run through scripts/check_cycle.py.
 */

#include <aegir/trinket/cycle.h>
#include <aegir/trinket/widget.h>
#include <aegir/trinket/window.h>

#include <cstdio>
#include <string>

namespace aegir::trinket {
/* widget.cc's damage path names the window, and cycle.cc's metrics name the
 * application; the check maps arithmetic but never draws, so a window that
 * swallows the damage and a null application are enough
 * (specs/trinket/cycle.md). window.h is host-includable through the sel4 stub
 * beside the layout check (scripts/layout_stub/sel4/sel4.h). */
void Window::damage(const Rect&) {}
/* cycle.cc's menu names the window's popup layer; the check never opens one, so
 * a window that takes the widget and forgets it is enough. */
void Window::open_popup(std::unique_ptr<Widget>, Rect) {}
void Window::close_popup() {}
Application* Application::instance() { return nullptr; }
Font* Application::default_font() { return nullptr; }
}  // namespace aegir::trinket

namespace {

using aegir::trinket::Cycle;

unsigned g_checks = 0;
unsigned g_failures = 0;

void check(bool ok, std::string const& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

void expect(int active, int count, int delta, bool wrap, int want, std::string const& what) {
    int const got = Cycle::step_index(active, count, delta, wrap);
    check(got == want, what + ": got " + std::to_string(got) + ", want " + std::to_string(want));
}

}  // namespace

int main() {
    /* Forward and back in the middle. */
    expect(0, 3, 1, true, 1, "the first advances to the second");
    expect(1, 3, -1, true, 0, "the second goes back to the first");

    /* A wrap at both ends: what makes it a cycle. */
    expect(2, 3, 1, true, 0, "the last wraps to the first");
    expect(0, 3, -1, true, 2, "the first wraps to the last");

    /* A chain told not to wrap clamps. */
    expect(2, 3, 1, false, 2, "the last clamps without wrap");
    expect(0, 3, -1, false, 0, "the first clamps without wrap");

    /* A one-entry list is still one entry. */
    expect(0, 1, 1, true, 0, "a single entry wraps to itself");
    expect(0, 1, -1, true, 0, "a single entry goes back to itself");

    /* An empty list has no entry to step to. */
    expect(0, 0, 1, true, 0, "an empty list has no entry");

    /* A multi-step delta wraps as one. */
    expect(0, 5, 7, true, 2, "a long step wraps");
    expect(4, 5, -7, true, 2, "a long step back wraps");

    std::printf("cycle: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
