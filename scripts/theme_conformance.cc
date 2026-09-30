/*
 * Host render of the toolkit's gadgets (specs/trinket/theme-xen.md).
 *
 * Draws every gadget the theme owns onto one canvas with the toolkit's own
 * Theme and Canvas -- the same code the target runs -- and writes it as a PPM.
 * This is the fast loop: a gadget change is seen in seconds on the host instead
 * of a ten-minute boot, and the preview can be laid beside the reference art.
 *
 * Host tools only (python3 and a C++ compiler); not part of any target build.
 * Run through scripts/check_theme.py.
 */

#include <aegir/trinket/canvas.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/window.h>

#include <cstdio>
#include <cstdint>
#include <memory>
#include <vector>

namespace aegir::trinket {
/* The theme's GROUP_BOX title reads the application's font and the canvas names
 * no other application symbol; the render stubs both, as the other host checks
 * do (window.h is host-includable through scripts/layout_stub/sel4/sel4.h). */
void Window::damage(const Rect&) {}
Application* Application::instance() { return nullptr; }
Font* Application::default_font() { return nullptr; }
}  // namespace aegir::trinket

namespace {

using namespace aegir::trinket;

/* The scrollbar's parts, mirroring Scrollbar::parts() so the render shows the
 * same geometry the widget draws (frame 3, buttons at the foot). */
struct Parts {
    Rect trough, thumb, decrement, increment;
};

Parts scrollbar_parts(Rect const& rect, int total, int page, int value, int arrow,
                      int min_handle) {
    int const well = 4;  // the trough container's frame, matching the theme
    int const buttons = 2 * arrow;
    Parts p;
    p.decrement = Rect{rect.x, rect.y + rect.height - buttons, rect.width, arrow};
    p.increment = Rect{rect.x, rect.y + rect.height - arrow, rect.width, arrow};
    p.trough = Rect{rect.x, rect.y, rect.width, rect.height - buttons};
    Rect const well_rect = p.trough.inflated(-well);
    int const track = well_rect.height;
    int size = track * page / total;
    if (size < min_handle) size = min_handle;
    if (size > track) size = track;
    int const travel = track - size;
    int pos = travel <= 0 ? 0 : travel * value / (total - page);
    p.thumb = Rect{well_rect.x, well_rect.y + pos, well_rect.width, size};
    return p;
}

}  // namespace

int main(int argc, char** argv) {
    char const* path = argc > 1 ? argv[1] : "render.ppm";

    constexpr int kWidth = 420;
    constexpr int kHeight = 260;
    std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight, 0);
    Canvas canvas(pixels.data(), kWidth, kHeight, kWidth);

    std::unique_ptr<Theme> theme = Theme::create_xen(1.0f);
    canvas.fill_rect({0, 0, kWidth, kHeight}, Color(0x00AAAAAA));

    /* Buttons: normal, hovered, pressed, focused, disabled. */
    theme->draw_button(canvas, {10, 10, 90, 26}, false, false, false, false, true);
    theme->draw_button(canvas, {10, 46, 90, 26}, true, false, false, false, true);
    theme->draw_button(canvas, {10, 82, 90, 26}, false, true, false, false, true);
    theme->draw_button(canvas, {10, 118, 90, 26}, false, false, true, false, true);
    theme->draw_button(canvas, {10, 154, 90, 26}, false, false, false, false, false);

    /* Text fields: unfocused and focused. */
    theme->draw_textbox(canvas, {120, 10, 120, 26}, false, false, false);
    theme->draw_textbox(canvas, {120, 46, 120, 26}, true, false, false);
    theme->draw_textbox(canvas, {120, 82, 120, 26}, false, true, false);

    /* Panels: raised, sunken, frame. */
    theme->draw_panel(canvas, {260, 10, 100, 50}, Panel::Style::RAISED, U"", false);
    theme->draw_panel(canvas, {260, 70, 100, 50}, Panel::Style::SUNKEN, U"", false);
    theme->draw_panel(canvas, {260, 130, 100, 50}, Panel::Style::FRAME, U"", false);

    /* A vertical scrollbar, content 100 lines, 25 shown, scrolled to line 40. */
    Parts const p = scrollbar_parts({386, 10, 24, 200}, 100, 25, 40, 21, 30);
    theme->draw_scrollbar(canvas, {386, 10, 24, 200}, true, p.trough, p.thumb,
                          p.decrement, p.increment, false);

    /* The toggle indicators: a checkmark button and a radio ring, off and on. */
    theme->draw_check(canvas, {10, 198, 23, 18}, false, true);
    theme->draw_check(canvas, {45, 198, 23, 18}, true, true);
    theme->draw_radio(canvas, {85, 201, 17, 12}, false, true);
    theme->draw_radio(canvas, {115, 201, 17, 12}, true, true);

    std::FILE* out = std::fopen(path, "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    std::fprintf(out, "P6\n%d %d\n255\n", kWidth, kHeight);
    for (uint32_t const pixel : pixels) {
        uint8_t const rgb[3] = {static_cast<uint8_t>((pixel >> 16) & 0xFF),
                                static_cast<uint8_t>((pixel >> 8) & 0xFF),
                                static_cast<uint8_t>(pixel & 0xFF)};
        std::fwrite(rgb, 1, 3, out);
    }
    std::fclose(out);
    return 0;
}
