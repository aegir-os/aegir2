/*
 * Trinket's theme engine: it reads the palette, the metrics and the gadget
 * recipes from the generated theme data and draws with them
 * (specs/trinket/theming.md). Nothing here is a look of its own -- the XEN look
 * is resources/themes/xen.toml -- so a theme is data and this is the one
 * interpreter that draws it.
 */

#include <aegir/trinket/theme.h>
#include <aegir/trinket/theme_data.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/unicode.h>
#include <algorithm>
#include <memory>

namespace aegir::trinket {

class DataTheme : public Theme {
public:
    explicit DataTheme(float scale = 1.0f) : scale_(scale) {}

    Color color(ColorRole role) const override {
        /* The palette is data (specs/trinket/theming.md): one colour per role,
         * in the ColorRole enum's order, from resources/themes/xen.toml. */
        return Color(kPalette[static_cast<int>(role)]);
    }

    int metric(MetricRole role) const override {
        /* The metrics are data (specs/trinket/theming.md): a base size per role
         * at 96 dpi, scaled here unless the file marks it fixed. */
        int const index = static_cast<int>(role);
        int const base = static_cast<int>(scale_ + 0.5f);
        return kMetricFixed[index] ? kMetrics[index] : kMetrics[index] * base;
    }

    Font* font() const override { return face(FontRole::DEFAULT); }
    Font* font_small() const override { return face(FontRole::SMALL); }
    Font* font_large() const override { return face(FontRole::LARGE); }
    Font* font_monospace() const override { return face(FontRole::MONOSPACE); }

    /* The grey 3-D edge (specs/trinket/theme-xen.md): 1px, square, hard-edged,
     * a light top-left and a dark bottom-right for a raised gadget and the
     * reverse for a sunken one, over whatever face the caller filled. */
    void draw_bevel(Canvas& canvas, const Rect& rect, Bevel bevel) override {
        Color const light = color(ColorRole::GADGET_HIGHLIGHT);
        Color const dark = color(ColorRole::GADGET_SHADOW);
        Color const top = bevel == Bevel::RAISED ? light : dark;
        Color const bottom = bevel == Bevel::RAISED ? dark : light;
        canvas.draw_hline(rect.x, rect.x + rect.width - 1, rect.y, top);
        canvas.draw_vline(rect.y, rect.y + rect.height - 1, rect.x, top);
        canvas.draw_hline(rect.x, rect.x + rect.width - 1,
                          rect.y + rect.height - 1, bottom);
        canvas.draw_vline(rect.y, rect.y + rect.height - 1,
                          rect.x + rect.width - 1, bottom);
    }

    /* The Amiga selection: one pixel of `fg` on one of `bg`, so a selected list
     * or cycle row needs no blend (specs/trinket/theme-xen.md). */
    void draw_dither(Canvas& canvas, const Rect& rect, Color fg, Color bg) override {
        canvas.fill_rect(rect, bg);
        Rect const clip = rect.intersected(canvas.clip_rect());
        uint32_t const on = fg.to_uint32();
        for (int y = clip.y; y < clip.y + clip.height; ++y) {
            for (int x = clip.x + ((y - rect.y) & 1); x < clip.x + clip.width;
                 x += 2) {
                canvas.pixel(x, y) = on;
            }
        }
    }

    void draw_button(Canvas& canvas, const Rect& rect,
                     bool hovered, bool pressed, bool focused,
                     bool checked, bool enabled) override {
        static_cast<void>(checked);  // checked buttons are the widget's to draw
        /* The state picks the recipe (specs/trinket/theming.md): disabled beats
         * pressed beats focused beats hovered beats normal. */
        int const state = !enabled ? 4 : pressed ? 2 : focused ? 3 : hovered ? 1 : 0;
        run(kRecipeButton[state], canvas, rect);
    }

    void draw_check(Canvas& canvas, const Rect& rect, bool checked,
                    bool enabled) override {
        static_cast<void>(enabled);  // the artwork has no disabled frame
        run(kRecipeCheck[checked ? 1 : 0], canvas, rect);
    }

    void draw_radio(Canvas& canvas, const Rect& rect, bool checked,
                    bool enabled) override {
        static_cast<void>(enabled);
        run(kRecipeRadio[checked ? 1 : 0], canvas, rect);
    }

    void draw_panel(Canvas& canvas, const Rect& rect,
                     Panel::Style style, std::u32string_view title,
                     bool focused) override {
        static_cast<void>(focused);  // the focus ring is drawn separately
        run(kRecipePanel[static_cast<int>(style)], canvas, rect);
        if (style != Panel::Style::GROUP_BOX || title.empty()) return;
        Font* const font = Application::instance()->default_font();
        if (font == nullptr) return;
        Size const size = font->measure(title);
        /* The caption is centred in the box and straddles the frame's top edge
         * (specs/trinket/layout.md), as MUI's does: the group hands the frame a
         * rectangle whose top edge is the border line, with half the caption's
         * band above it -- the whole band is inset from its children -- so the
         * line is notched around the caption and never drawn over it. */
        int const x = rect.x + (rect.width - size.width) / 2;
        int const y = rect.y - font->height() / 2;
        canvas.fill_rect({x - 2, y, size.width + 4, font->height()},
                         color(ColorRole::PANEL_BG));
        canvas.draw_text({x, y}, title, font, color(ColorRole::TEXT));
    }

    void draw_textbox(Canvas& canvas, const Rect& rect,
                       bool focused, bool read_only, bool password) override {
        static_cast<void>(password);  // the text is the widget's to obscure
        if (read_only) {
            run(kRecipeTextboxReadonly, canvas, rect);
        } else {
            run(kRecipeTextbox[focused ? 1 : 0], canvas, rect);
        }
    }

    void draw_screen_bar(Canvas& canvas, const Rect& rect) override {
        run(kRecipeScreenBar, canvas, rect);
    }

    void draw_menubar(Canvas& canvas, const Rect& rect) override {
        run(kRecipeMenubar, canvas, rect);
    }

    void draw_menu_well(Canvas& canvas, const Rect& rect) override {
        run(kRecipeMenuWell, canvas, rect);
    }

    void draw_menu_item(Canvas& canvas, const Rect& rect,
                         const char* label, bool hovered,
                         bool checked, bool disabled, bool separator,
                         bool submenu) override {
        if (separator) {
            int const y = rect.y + rect.height / 2;
            canvas.draw_hline(rect.x + 20, rect.x + rect.width - 20, y,
                              color(ColorRole::MENU_SEPARATOR));
            return;
        }

        run(kRecipeMenuItem[hovered && !disabled ? 1 : 0], canvas, rect);

        Color const text_color = disabled ? color(ColorRole::DISABLED_TEXT)
                                          : color(ColorRole::MENU_TEXT);
        Font* const font = Application::instance()->default_font();
        if (font != nullptr && label != nullptr) {
            int const x = rect.x + metric(MetricRole::MENU_PADDING_H);
            int const y = rect.y + (rect.height - font->height()) / 2;
            canvas.draw_text({x, y}, utf8_to_utf32(label), font, text_color);
        }

        if (checked) {
            /* The checkmark, the two strokes MUI's menu shows. */
            int const cx = rect.x + 4;
            int const cy = rect.y + rect.height / 2;
            canvas.draw_line({cx, cy}, {cx + 4, cy + 4}, text_color);
            canvas.draw_line({cx + 4, cy + 4}, {cx + 10, cy - 4}, text_color);
        }
        if (submenu) {
            /* A submenu's chevron: a right-pointing angle, the item's far right. */
            int const ax = rect.x + rect.width - metric(MetricRole::MENU_PADDING_H) - 10;
            int const ay = rect.y + rect.height / 2;
            canvas.draw_line({ax, ay - 4}, {ax + 6, ay}, text_color);
            canvas.draw_line({ax + 6, ay}, {ax, ay + 4}, text_color);
        }
    }

    void draw_keycap(Canvas& canvas, const Rect& rect, const char* label) override {
        run(kRecipeKeycap, canvas, rect);
        Font* const font = Application::instance()->default_font();
        if (font == nullptr || label == nullptr) {
            return;
        }
        int const pad = metric(MetricRole::MENU_PADDING_V);
        canvas.draw_text({rect.x + pad, rect.y + (rect.height - font->height()) / 2},
                         utf8_to_utf32(label), font, color(ColorRole::BUTTON_TEXT));
    }

    void draw_titlebar(Canvas& canvas, const Rect& rect,
                        const char* title, bool active,
                        bool close_gadget) override {
        run(kRecipeTitlebar[active ? 1 : 0], canvas, rect);
        if (title == nullptr) {
            return;
        }
        Font* const font = Application::instance()->default_font();
        if (font == nullptr) {
            return;
        }
        /* The title starts past the close gadget at the bar's far left, or at the
         * padding when there is none (specs/amiga-fidelity.md). */
        int x = rect.x + metric(MetricRole::TITLEBAR_PADDING_H);
        if (close_gadget) {
            x += metric(MetricRole::TITLEBAR_BUTTON_SIZE) +
                 metric(MetricRole::SPACING_SMALL);
        }
        int const y = rect.y + (rect.height - font->height()) / 2;
        canvas.draw_text({x, y}, utf8_to_utf32(title), font,
                         color(active ? ColorRole::TITLEBAR_TEXT
                                      : ColorRole::TITLEBAR_TEXT_INACTIVE));
    }

    void draw_gadget(Canvas& canvas, const Rect& rect,
                      GadgetKind kind, bool active) override {
        run(kRecipeGadget[static_cast<int>(kind) * 2 + (active ? 1 : 0)], canvas, rect);
    }

    void draw_bottombar(Canvas& canvas, const Rect& rect) override {
        run(kRecipeBottombar, canvas, rect);
    }

    /* The resize gadget: a white right triangle, near-black outline, right angle
     * at the bottom-right, inset from the bar's bevel, with a white line down its
     * left separating it from the bar (specs/amiga-fidelity.md). Its geometry has
     * no recipe form, so it stays here (specs/trinket/chrome.md). */
    void draw_resize_gadget(Canvas& canvas, const Rect& rect) override {
        Color const white = color(ColorRole::GADGET_WHITE);
        Color const outline = color(ColorRole::GADGET_OUTLINE);
        int const tri = (rect.width * 5) / 8;
        int const gap = (rect.width - tri) / 2;
        Rect const t{rect.x + rect.width - gap - tri, rect.y + rect.height - gap - tri,
                     tri, tri};
        canvas.draw_vline(rect.y, rect.y + rect.height - 1, rect.x, white);
        for (int row = 0; row < t.height; ++row) {
            int const left = t.x + (t.width - 1) * (t.height - 1 - row) /
                                       (t.height > 1 ? t.height - 1 : 1);
            canvas.draw_hline(left, t.x + t.width - 1, t.y + row, white);
        }
        canvas.draw_vline(t.y, t.y + t.height - 1, t.x + t.width - 1, outline);
        canvas.draw_hline(t.x, t.x + t.width - 1, t.y + t.height - 1, outline);
        canvas.draw_line({t.x + t.width - 1, t.y}, {t.x, t.y + t.height - 1}, outline);
    }

    void draw_window_frame(Canvas& canvas, const Rect& rect, bool active) override {
        static_cast<void>(active);  // the Workbench frame is the same either way
        run(kRecipeWindowFrame, canvas, rect);
    }

    void draw_scrollbar(Canvas& canvas, const Rect& rect, bool vertical,
                         const Rect& trough, const Rect& thumb,
                         const Rect& decrement, const Rect& increment,
                         bool decrement_pressed, bool increment_pressed) override {
        static_cast<void>(rect);  // the trough and the buttons are the whole strip
        /* Each part is a recipe (specs/trinket/theming.md); the buttons are
         * separate cells, their marks in their own recipes -- one per
         * orientation, and the MUI selected frame while the button is held
         * (specs/trinket/scrollbar.md). */
        if (trough.width > 0 && trough.height > 0) {
            run(kRecipeScrollbarTrough, canvas, trough);
        }
        if (thumb.width > 0 && thumb.height > 0) {
            run(kRecipeScrollbarThumb, canvas, thumb);
        }
        if (decrement.width > 0 && decrement.height > 0) {
            Recipe const* const recipes =
                vertical ? kRecipeScrollbarDecrement : kRecipeScrollbarDecrementHorizontal;
            run(recipes[decrement_pressed ? 1 : 0], canvas, decrement);
        }
        if (increment.width > 0 && increment.height > 0) {
            Recipe const* const recipes =
                vertical ? kRecipeScrollbarIncrement : kRecipeScrollbarIncrementHorizontal;
            run(recipes[increment_pressed ? 1 : 0], canvas, increment);
        }
    }

    void draw_slider(Canvas& canvas, const Rect& trough, const Rect& knob,
                     bool hovered) override {
        static_cast<void>(hovered);  // the XEN knob has no hover state
        /* The trough and the knob are the widget's geometry and each is its own
         * recipe (specs/trinket/slider.md); the recipes start as the bar's. */
        if (trough.width > 0 && trough.height > 0) {
            run(kRecipeSliderTrough, canvas, trough);
        }
        if (knob.width > 0 && knob.height > 0) {
            run(kRecipeSliderKnob, canvas, knob);
        }
    }

    void draw_cycle(Canvas& canvas, const Rect& rect, const Rect& divider,
                    const Rect& mark, bool pressed, bool hovered) override {
        static_cast<void>(hovered);  // the XEN cycle has no hover state
        /* The boxed face, the divider and the mark (specs/trinket/cycle.md). */
        run(pressed ? kRecipeCyclePressed : kRecipeCycleFace, canvas, rect);
        if (divider.width > 0 && divider.height > 0) {
            run(kRecipeCycleDivider, canvas, divider);
        }
        if (mark.width > 0 && mark.height > 0) {
            run(kRecipeCycleMark, canvas, mark);
        }
    }

    void draw_popup(Canvas& canvas, const Rect& rect, PopupButton::Role role,
                    bool selected) override {
        /* The image is the whole button, in its normal or selected frame
         * (specs/trinket/popup_button.md): role * 2 picks the pair. */
        int const index = static_cast<int>(role) * 2 + (selected ? 1 : 0);
        if (index < 0 || index >= 6) return;
        run(kRecipePopup[index], canvas, rect);
    }

    void draw_list(Canvas& canvas, const Rect& rect) override {
        /* The well: a black outline around the gadget face
         * (specs/trinket/listview.md). */
        run(kRecipeListWell, canvas, rect);
    }

    void draw_list_row(Canvas& canvas, const Rect& rect, ListRow state) override {
        /* The row being pointed at is the dither and the chosen one a solid bar
         * (specs/trinket/theme-xen.md); a plain row shows the well's face. */
        if (state == ListRow::CURSOR) {
            run(kRecipeListCursor, canvas, rect);
        } else if (state == ListRow::SELECTED) {
            run(kRecipeListSelected, canvas, rect);
        }
    }

    void draw_list_header(Canvas& canvas, const Rect& rect) override {
        run(kRecipeListHeader, canvas, rect);
    }

    void draw_selection(Canvas& canvas, const Rect& rect) override {
        run(kRecipeSelection, canvas, rect);
    }

    void draw_caret(Canvas& canvas, const Rect& rect) override {
        run(kRecipeCaret, canvas, rect);
    }

    void draw_icon(Canvas& canvas, const Rect& rect, Icon icon) override {
        /* The imported MUI artwork, one recipe per role (specs/trinket/listview.md).
         * The art is a small wide bitmap, so it is fitted into the cell rather
         * than stretched to it. Icon::NONE is the empty recipe and draws
         * nothing. */
        int const index = static_cast<int>(icon);
        if (index < 0 || index >= 7) return;
        Recipe const& recipe = kRecipeIcon[index];
        if (recipe.count == 0 || recipe.steps[0].op != Prim::SPRITE) return;
        run(recipe, canvas, fit_sprite(kSprites[recipe.steps[0].sprite], rect));
    }

    void draw_tab(Canvas& canvas, const Rect& rect, std::u32string_view title,
                  bool active, bool hovered) override {
        /* A chamfered face, the title centred (specs/trinket/tabs.md). The face
         * is the group's own (PANEL_BG), so a tab and the body are one colour
         * and the bevel alone says where the tab is; what a page paints inside
         * is the app's. The face is filled row by row so the two top corners
         * are cut and the raised edge follows the cut; the active tab leaves
         * its bottom edge open where it meets the body, so the two read as
         * one. */
        static_cast<void>(hovered);  // the XEN tab has no hover state
        int const cut = metric(MetricRole::TAB_CHAMFER);
        Color const face = color(ColorRole::PANEL_BG);
        Color const light = color(ColorRole::GADGET_HIGHLIGHT);
        Color const dark = color(ColorRole::GADGET_SHADOW);
        for (int row = 0; row < rect.height; ++row) {
            int const step = cut - row;
            int const inset = step > 0 ? step : 0;
            int const width = rect.width - 2 * inset;
            if (width <= 0) continue;
            canvas.fill_rect({rect.x + inset, rect.y + row, width, 1}, face);
        }
        int const top = rect.y;
        int const bottom = rect.y + rect.height - 1;
        int const last = rect.x + rect.width - 1;
        canvas.draw_line({rect.x, top + cut}, {rect.x + cut, top}, light);
        canvas.draw_hline(rect.x + cut, last - cut, top, light);
        canvas.draw_vline(top + cut, bottom, rect.x, light);
        canvas.draw_line({last - cut, top}, {last, top + cut}, dark);
        canvas.draw_vline(top + cut, bottom, last, dark);
        if (!active) {
            canvas.draw_hline(rect.x, last, bottom, dark);
        }
        Font* const font = Application::instance()->default_font();
        if (font == nullptr || title.empty()) return;
        Size const text = font->measure(title);
        canvas.draw_text({rect.x + (rect.width - text.width) / 2,
                          rect.y + (rect.height - font->height()) / 2},
                         title, font, color(ColorRole::TEXT));
    }

    /* The editor's caret (specs/trinket/editor.md): insert mode is a block
     * filled in the Workbench blue, overwrite an underline along the cell's
     * foot. The widget computes the cell and draws any glyph over the block. */
    void draw_cursor(Canvas& canvas, const Rect& cell, CursorShape shape) override {
        Color const ink = color(ColorRole::CURSOR_BG);
        if (shape == CursorShape::BLOCK) {
            canvas.fill_rect(cell, ink);
            return;
        }
        int const thickness = cell.height >= 8 ? 2 : 1;
        canvas.fill_rect({cell.x, cell.y + cell.height - thickness, cell.width,
                          thickness}, ink);
    }

    /* Focus is the gadget's own active state (specs/trinket/theme-xen.md): the
     * button and the field draw it, so there is no separate ring. */
    void draw_focus_ring(Canvas&, const Rect&) override {}

    void draw_tooltip(Canvas& canvas, const Rect& rect,
                       const char* text) override {
        canvas.fill_rounded_rect(rect, 4, color(ColorRole::TOOLTIP_BG));
        Font* font = Application::instance()->default_font();
        if (font && text) {
            int x = rect.x + 6;
            int y = rect.y + (rect.height - font->height()) / 2;
            canvas.draw_text({x, y}, utf8_to_utf32(text),
                             font, color(ColorRole::TOOLTIP_TEXT));
        }
    }

private:
    /* The face a role names, loaded through the application -- Sys:Fonts' BDF
     * faces and the font service's OpenType ones (specs/fonts.md). The host
     * render has no application, and so no font. */
    Font* face(FontRole role) const {
        Application* const app = Application::instance();
        if (app == nullptr) {
            return nullptr;
        }
        FontSpec const& spec = kFonts[static_cast<int>(role)];
        return app->font_for(spec.family, spec.size);
    }

    /* Run a gadget's recipe (specs/trinket/theming.md): each step draws one
     * primitive into the gadget's rectangle, set in by its inset and narrowed to
     * its `at` sub-rectangle (specs/trinket/chrome.md). */
    void run(Recipe const& recipe, Canvas& canvas, Rect const& rect) {
        for (int i = 0; i < recipe.count; ++i) {
            Step const& step = recipe.steps[i];
            Rect const r = sub_rect(rect, step);
            switch (step.op) {
                case Prim::FILL:
                    canvas.fill_rect(r, Color(step.color));
                    break;
                case Prim::BEVEL:
                    draw_bevel(canvas, r, step.kind ? Bevel::SUNKEN : Bevel::RAISED);
                    break;
                case Prim::OUTLINE:
                    canvas.draw_rect(r, Color(step.color));
                    break;
                case Prim::DITHER:
                    draw_dither(canvas, r, Color(step.color), Color(step.color2));
                    break;
                case Prim::MARK:
                    draw_mark(canvas, r, step.kind, Color(step.color), Color(step.color2),
                              step.num, step.den);
                    break;
                case Prim::LINE:
                    draw_edge(canvas, r, step.kind, Color(step.color));
                    break;
                case Prim::SPRITE:
                    blit_sprite(canvas, r, step.sprite);
                    break;
            }
        }
    }

    /* The step's rectangle: the gadget set in by its inset, then narrowed to its
     * `at` sub-rectangle in sixteenths -- the whole of it by default. The
     * sub-rectangle is what offsets and nests a gadget's glyph inside its cell,
     * which a uniform inset cannot (specs/trinket/chrome.md). */
    static Rect sub_rect(Rect const& rect, Step const& step) {
        Rect const r = rect.inflated(-step.inset);
        if (step.x0 == 0 && step.y0 == 0 && step.x1 == 16 && step.y1 == 16) {
            return r;
        }
        int const x0 = r.x + r.width * step.x0 / 16;
        int const y0 = r.y + r.height * step.y0 / 16;
        int const x1 = r.x + r.width * step.x1 / 16;
        int const y1 = r.y + r.height * step.y1 / 16;
        return {x0, y0, x1 - x0, y1 - y0};
    }

    /* One edge of the rectangle (specs/trinket/chrome.md): kind 0 top, 1 bottom,
     * 2 left, 3 right. The bars' light and dark lines are single edges, not the
     * four-edge bevel a gadget wears. */
    static void draw_edge(Canvas& canvas, Rect const& r, int kind, Color color) {
        switch (kind) {
        case 0: canvas.draw_hline(r.x, r.x + r.width - 1, r.y, color); break;
        case 1: canvas.draw_hline(r.x, r.x + r.width - 1, r.y + r.height - 1, color); break;
        case 2: canvas.draw_vline(r.y, r.y + r.height - 1, r.x, color); break;
        default: canvas.draw_vline(r.y, r.y + r.height - 1, r.x + r.width - 1, color); break;
        }
    }

    /* A hollow 3-D triangle: the leading (left) edge light and the other two
     * edges dark, inset from the rectangle -- the mark the XEN art shows
     * (specs/trinket/scrollbar.md). */
    void draw_mark(Canvas& canvas, const Rect& rect, int kind, Color light,
                   Color dark, int num, int den) {
        int const short_side = std::min(rect.width, rect.height);
        if (short_side <= 0 || den <= 0) return;
        int const half = std::max(1, short_side * num / den);
        Point const c = rect.center();
        bool const vertical = kind <= 1;
        bool const forward = kind == 1 || kind == 3;  // down or right
        if (vertical) {
            int const base = forward ? c.y - half : c.y + half;
            int const apex = forward ? c.y + half : c.y - half;
            Point const apex_pt{c.x, apex};
            Point const left{c.x - half, base};
            Point const right{c.x + half, base};
            canvas.draw_line(left, apex_pt, light);
            canvas.draw_line(apex_pt, right, dark);
            canvas.draw_line(left, right, dark);
        } else {
            int const base = forward ? c.x - half : c.x + half;
            int const apex = forward ? c.x + half : c.x - half;
            Point const apex_pt{apex, c.y};
            Point const top{base, c.y - half};
            Point const bottom{base, c.y + half};
            canvas.draw_line(top, apex_pt, light);
            canvas.draw_line(apex_pt, bottom, dark);
            canvas.draw_line(top, bottom, dark);
        }
    }

    /* The largest rectangle of the sprite's aspect that fits `rect`, centered:
     * an icon's art is a small wide bitmap, and stretching it to a square cell
     * squashes it (specs/trinket/listview.md). */
    static Rect fit_sprite(Sprite const& sprite, Rect const& rect) {
        if (sprite.width == 0 || sprite.height == 0 || rect.width <= 0 || rect.height <= 0) {
            return rect;
        }
        int w = rect.width;
        int h = w * sprite.height / sprite.width;
        if (h > rect.height) {
            h = rect.height;
            w = h * sprite.width / sprite.height;
        }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        return {rect.x + (rect.width - w) / 2, rect.y + (rect.height - h) / 2, w, h};
    }

    /* Blit a sprite into the rectangle, scaled to it (nearest neighbour) --
     * the art is at one point size and the bar another, so the buttons keep
     * their proportion -- skipping transparent pixels and clipping to the
     * rectangle and the canvas (specs/trinket/theming.md). */
    void blit_sprite(Canvas& canvas, const Rect& rect, int index) {
        if (index < 0 || index >= static_cast<int>(kSpriteCount)) return;
        Sprite const& sprite = kSprites[index];
        if (sprite.width == 0 || sprite.height == 0) return;
        Rect const clip = rect.intersected(canvas.clip_rect());
        if (clip.empty()) return;
        for (int py = clip.y; py < clip.y + clip.height; ++py) {
            int const sy = (py - rect.y) * sprite.height / rect.height;
            for (int px = clip.x; px < clip.x + clip.width; ++px) {
                int const sx = (px - rect.x) * sprite.width / rect.width;
                uint32_t const pixel = sprite.pixels[sy * sprite.width + sx];
                if ((pixel >> 24) == 0) continue;
                canvas.pixel(px, py) = pixel & 0xFFFFFFu;
            }
        }
    }

    float scale_ = 1.0f;
};

std::unique_ptr<Theme> Theme::create_xen(float scale) {
    return std::make_unique<DataTheme>(scale);
}

std::unique_ptr<Theme> Theme::create_high_contrast(float scale) {
    // TODO: High contrast theme
    return create_xen(scale);
}

} // namespace aegir::trinket