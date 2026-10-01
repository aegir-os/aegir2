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
 * same geometry the widget draws: the trough's frame, and the two arrow cells
 * at the far end of the run -- below a vertical bar, right of a horizontal one. */
struct Parts {
    Rect trough, thumb, decrement, increment;
};

Parts scrollbar_parts(Rect const& rect, bool vertical, int total, int page, int value,
                      int arrow, int min_handle) {
    int const well = 4;  // the trough container's frame, matching the theme
    int const buttons = 2 * arrow;
    Parts p;
    if (vertical) {
        p.decrement = Rect{rect.x, rect.y + rect.height - buttons, rect.width, arrow};
        p.increment = Rect{rect.x, rect.y + rect.height - arrow, rect.width, arrow};
        p.trough = Rect{rect.x, rect.y, rect.width, rect.height - buttons};
    } else {
        p.decrement = Rect{rect.x + rect.width - buttons, rect.y, arrow, rect.height};
        p.increment = Rect{rect.x + rect.width - arrow, rect.y, arrow, rect.height};
        p.trough = Rect{rect.x, rect.y, rect.width - buttons, rect.height};
    }
    Rect const well_rect = p.trough.inflated(-well);
    int const track = vertical ? well_rect.height : well_rect.width;
    int size = track * page / total;
    if (size < min_handle) size = min_handle;
    if (size > track) size = track;
    int const travel = track - size;
    int pos = travel <= 0 ? 0 : travel * value / (total - page);
    p.thumb = vertical ? Rect{well_rect.x, well_rect.y + pos, well_rect.width, size}
                       : Rect{well_rect.x + pos, well_rect.y, size, well_rect.height};
    return p;
}

}  // namespace

int main(int argc, char** argv) {
    char const* path = argc > 1 ? argv[1] : "render.ppm";

    constexpr int kWidth = 460;
    constexpr int kHeight = 444;
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

    /* A vertical scrollbar, content 100 lines, 25 shown, scrolled to line 40,
     * and its twin with the increment arrow held -- the MUI selected frame
     * (specs/trinket/scrollbar.md). */
    Rect const vbar{386, 10, 24, 200};
    Parts const p = scrollbar_parts(vbar, true, 100, 25, 40, 21, 30);
    theme->draw_scrollbar(canvas, vbar, true, p.trough, p.thumb, p.decrement,
                          p.increment, false, false);
    Rect const vbar_held{416, 10, 24, 200};
    Parts const p_held = scrollbar_parts(vbar_held, true, 100, 25, 40, 21, 30);
    theme->draw_scrollbar(canvas, vbar_held, true, p_held.trough, p_held.thumb,
                          p_held.decrement, p_held.increment, false, true);

    /* The toggle indicators: a checkmark button and a radio ring, off and on. */
    theme->draw_check(canvas, {10, 198, 23, 18}, false, true);
    theme->draw_check(canvas, {45, 198, 23, 18}, true, true);
    theme->draw_radio(canvas, {85, 201, 17, 12}, false, true);
    theme->draw_radio(canvas, {115, 201, 17, 12}, true, true);

    /* A horizontal slider over 0..100, a quarter along: the bar's trough and
     * knob at the slider's own geometry (specs/trinket/slider.md). */
    Rect const slider_trough{10, 228, 180, 24};
    Rect const slider_inner = slider_trough.inflated(-4);
    int const slider_knob = 16;
    int const slider_pos = (slider_inner.width - slider_knob) * 25 / 100;
    theme->draw_slider(canvas, slider_trough,
                       {slider_inner.x + slider_pos, slider_inner.y, slider_knob,
                        slider_inner.height},
                       false);

    /* A cycle beside it: the boxed face, its divider and the mark, which are
     * the theme's part (specs/trinket/cycle.md). */
    Rect const cycle{200, 228, 100, 24};
    Rect const divider{cycle.x + 1 + 20, cycle.y + 1, 1, cycle.height - 2};
    int const mark = 8;
    theme->draw_cycle(canvas, cycle, divider,
                      {cycle.x + 1 + (20 - mark) / 2, cycle.y + 1 + (cycle.height - 2 - mark) / 2,
                       mark, mark},
                      false, false);

    /* The popup button's three roles, normal over selected: the MUI image is the
     * whole button (specs/trinket/popup_button.md). */
    theme->draw_popup(canvas, {312, 228, 22, 17}, PopupButton::Role::POPUP, false);
    theme->draw_popup(canvas, {340, 228, 22, 17}, PopupButton::Role::FILE, false);
    theme->draw_popup(canvas, {368, 228, 22, 17}, PopupButton::Role::DRAWER, false);
    theme->draw_popup(canvas, {312, 250, 22, 17}, PopupButton::Role::POPUP, true);
    theme->draw_popup(canvas, {340, 250, 22, 17}, PopupButton::Role::FILE, true);
    theme->draw_popup(canvas, {368, 250, 22, 17}, PopupButton::Role::DRAWER, true);

    /* A list: the well, then a plain row, the row being pointed at (the dither)
     * and the chosen row (the solid bar), which are the theme's own rows
     * (specs/trinket/listview.md). */
    Rect const list{10, 278, 200, 66};
    theme->draw_list(canvas, list);
    Rect const row_area = list.inflated(-1);
    int const row_h = row_area.height / 3;
    theme->draw_list_row(canvas, {row_area.x, row_area.y, row_area.width, row_h},
                         Theme::ListRow::NORMAL);
    theme->draw_list_row(canvas, {row_area.x, row_area.y + row_h, row_area.width, row_h},
                         Theme::ListRow::CURSOR);
    theme->draw_list_row(canvas, {row_area.x, row_area.y + 2 * row_h, row_area.width, row_h},
                         Theme::ListRow::SELECTED);

    /* Horizontal scrollbars: the same widget turned, drawing the MUI
     * ArrowLeft/ArrowRight art -- normal, the decrement arrow held and the
     * increment held, so both selected frames are on the sheet. */
    Rect const hbar{10, 352, 200, 24};
    Parts const hp = scrollbar_parts(hbar, false, 100, 25, 40, 21, 30);
    theme->draw_scrollbar(canvas, hbar, false, hp.trough, hp.thumb, hp.decrement,
                          hp.increment, false, false);
    Rect const hbar_dec{230, 352, 200, 24};
    Parts const hp_dec = scrollbar_parts(hbar_dec, false, 100, 25, 40, 21, 30);
    theme->draw_scrollbar(canvas, hbar_dec, false, hp_dec.trough, hp_dec.thumb,
                          hp_dec.decrement, hp_dec.increment, true, false);
    Rect const hbar_inc{10, 384, 200, 24};
    Parts const hp_inc = scrollbar_parts(hbar_inc, false, 100, 25, 40, 21, 30);
    theme->draw_scrollbar(canvas, hbar_inc, false, hp_inc.trough, hp_inc.thumb,
                          hp_inc.decrement, hp_inc.increment, false, true);

    /* The row icons (specs/trinket/listview.md): the imported MUI drawer and
     * volume artwork, one per role. */
    Icon const icons[] = {Icon::DRAWER, Icon::HARD_DISK, Icon::DISK,
                          Icon::CHIP, Icon::VOLUME, Icon::NETWORK};
    int ix = 10;
    for (Icon const icon : icons) {
        theme->draw_icon(canvas, {ix, 416, 16, 16}, icon);
        ix += 20;
    }

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
