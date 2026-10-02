/*
 * Trinket Theme system.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Pluggable theme with XEN/Workbench default.
 */

#ifndef AEGIR_TRINKET_THEME_H
#define AEGIR_TRINKET_THEME_H

#include <aegir/trinket/color.h>
#include <aegir/trinket/font.h>
#include <aegir/trinket/icon.h>
#include <aegir/trinket/panel.h>
#include <aegir/trinket/popup_button.h>
#include <memory>
#include <string_view>

namespace aegir::trinket {

class Canvas;
class Widget;

enum class ColorRole {
    // Base
    BACKGROUND, WINDOW_BG, PANEL_BG,
    // Text
    TEXT, TEXT_DISABLED, TEXT_SELECTED, TEXT_INVERSE,
    // Buttons
    BUTTON_BG, BUTTON_HOVER, BUTTON_PRESSED, BUTTON_FOCUS, BUTTON_TEXT, BUTTON_BORDER,
    // Input
    INPUT_BG, INPUT_TEXT, INPUT_PLACEHOLDER, INPUT_BORDER, INPUT_FOCUS_BORDER,
    // Selection
    SELECTION_BG, SELECTION_TEXT,
    // The editor's caret (specs/trinket/editor.md): the blue a block insert
    // mode fills and an underline overwrite mode draws its line in, and the
    // colour a glyph under a block is drawn in.
    CURSOR_BG, CURSOR_TEXT,
    // Accent
    ACCENT, ACCENT_HOVER, ACCENT_PRESSED,
    // Borders
    BORDER, BORDER_LIGHT, BORDER_DARK, FOCUS_BORDER,
    // Menu
    MENUBAR_BG, MENUBAR_HOVER, MENUBAR_TEXT,
    MENU_BG, MENU_HOVER, MENU_TEXT, MENU_BORDER, MENU_SEPARATOR,
    // Titlebar
    TITLEBAR_BG, TITLEBAR_BG_INACTIVE, TITLEBAR_TEXT, TITLEBAR_TEXT_INACTIVE,
    TITLEBAR_BUTTON_BG, TITLEBAR_BUTTON_HOVER,
    // Workbench chrome (specs/amiga-fidelity.md): the bars' bevels, the
    // gadget fills, and the frame's raised edge.
    TITLEBAR_HIGHLIGHT, TITLEBAR_SHADOW,
    BOTTOMBAR_HIGHLIGHT, BOTTOMBAR_SHADOW,
    GADGET_OUTLINE, GADGET_GREY, GADGET_WHITE,
    FRAME_LIGHT, FRAME_DARK,
    // Grey 3-D gadgets (specs/trinket/theme-xen.md): the face and the two bevel
    // edges a button, a field or a group frame is drawn from.
    GADGET_FACE, GADGET_HIGHLIGHT, GADGET_SHADOW, GADGET_SOFT_SHADOW,
    // Tooltip
    TOOLTIP_BG, TOOLTIP_TEXT,
    // Scrollbar
    SCROLLBAR_BG, SCROLLBAR_HANDLE, SCROLLBAR_HANDLE_HOVER,
    // Link
    LINK, LINK_VISITED, LINK_HOVER,
    // Error/Warning/Success
    ERROR, WARNING, SUCCESS, INFO,
    // Disabled
    DISABLED_BG, DISABLED_TEXT
};

enum class MetricRole {
    // Buttons
    BUTTON_PADDING_H, BUTTON_PADDING_V,
    BUTTON_MIN_WIDTH, BUTTON_MIN_HEIGHT,
    BUTTON_BORDER_WIDTH, BUTTON_RADIUS,
    // Panels
    PANEL_BORDER_WIDTH, PANEL_RADIUS,
    // Menu
    MENUBAR_HEIGHT, MENU_ITEM_HEIGHT, MENU_PADDING_H, MENU_PADDING_V,
    MENU_SEPARATOR_HEIGHT, MENU_BORDER_WIDTH,
    // Titlebar
    TITLEBAR_HEIGHT, TITLEBAR_BUTTON_SIZE, TITLEBAR_PADDING_H,
    // Window
    WINDOW_BORDER_WIDTH, WINDOW_SHADOW_WIDTH,
    // Input
    INPUT_PADDING_H, INPUT_PADDING_V, INPUT_BORDER_WIDTH,
    // Scrollbar
    SCROLLBAR_WIDTH, SCROLLBAR_MIN_HANDLE, SCROLLBAR_ARROW_SIZE,
    // Slider (specs/trinket/slider.md): the trough's thickness, the knob's
    // length along the track, and a shortest useful strip.
    SLIDER_THICKNESS, SLIDER_KNOB_LENGTH, SLIDER_MIN_LENGTH,
    // Cycle and popup button (specs/trinket/cycle.md, popup_button.md): the
    // cycle's button cell and its mark, and the popup button's own size.
    CYCLE_BUTTON_WIDTH, CYCLE_MARK_SIZE, POPUP_BUTTON_WIDTH, POPUP_BUTTON_HEIGHT,
    // List (specs/trinket/listview.md): the row's padding, the text's inset and
    // the width and row count a list asks for. The row's height is the font's
    // line plus twice LIST_ROW_PADDING_V, so it follows the font.
    LIST_ROW_PADDING_V, LIST_PADDING_H, LIST_MIN_WIDTH, LIST_PREFERRED_ROWS,
    // Toggle gadgets (specs/trinket/checkbox.md): the checkmark's and the
    // radio's indicator, the MUI artwork's own size.
    CHECK_INDICATOR_WIDTH, CHECK_INDICATOR_HEIGHT,
    RADIO_INDICATOR_WIDTH, RADIO_INDICATOR_HEIGHT,
    // Tabs (specs/trinket/tabs.md): a tab's horizontal padding, the strip's
    // height, the gap between tabs, and the 45-degree chamfer cut from each
    // tab's two top corners.
    TAB_PADDING_H, TAB_HEIGHT, TAB_GAP, TAB_CHAMFER,
    // General
    SPACING_SMALL, SPACING_MEDIUM, SPACING_LARGE,
    FOCUS_RING_WIDTH, FOCUS_RING_OFFSET,
    ICON_SIZE_SMALL, ICON_SIZE_NORMAL, ICON_SIZE_LARGE,
    TOOLTIP_DELAY_MS,
    // Text
    TEXT_LINE_HEIGHT_MULTIPLIER
};

class Theme {
public:
    virtual ~Theme() = default;

    virtual Color color(ColorRole role) const = 0;
    virtual int metric(MetricRole role) const = 0;
    virtual Font* font() const = 0;
    virtual Font* font_small() const = 0;
    virtual Font* font_large() const = 0;
    virtual Font* font_monospace() const = 0;

    // The grey 3-D edge (specs/trinket/theme-xen.md): 1px, square, hard-edged.
    enum class Bevel { RAISED, SUNKEN };

    // Drawing primitives (can be overridden for custom look)
    virtual void draw_bevel(Canvas& canvas, const Rect& rect, Bevel bevel);
    virtual void draw_dither(Canvas& canvas, const Rect& rect, Color fg, Color bg);
    virtual void draw_button(Canvas& canvas, const Rect& rect,
                              bool hovered, bool pressed, bool focused,
                              bool checked, bool enabled);
    // The toggle gadgets' indicators (specs/trinket/checkbox.md): the MUI
    // checkmark or radio artwork, in its unchecked or checked frame. The rect is
    // the indicator's own.
    virtual void draw_check(Canvas& canvas, const Rect& rect, bool checked,
                             bool enabled);
    virtual void draw_radio(Canvas& canvas, const Rect& rect, bool checked,
                             bool enabled);
    virtual void draw_panel(Canvas& canvas, const Rect& rect,
                             Panel::Style style, std::u32string_view title,
                             bool focused);
    virtual void draw_textbox(Canvas& canvas, const Rect& rect,
                               bool focused, bool read_only, bool password);
    virtual void draw_menubar(Canvas& canvas, const Rect& rect);
    virtual void draw_menu_item(Canvas& canvas, const Rect& rect,
                                 const char* label, bool hovered,
                                 bool checked, bool disabled, bool separator);
    // The window chrome (specs/trinket/chrome.md): the widget computes the
    // geometry and the theme draws the look. `close_gadget` tells the title bar
    // where the title starts, past the close gadget at its far left.
    virtual void draw_titlebar(Canvas& canvas, const Rect& rect,
                                const char* title, bool active,
                                bool close_gadget);
    enum class GadgetKind { CLOSE, ZOOM, DEPTH };
    virtual void draw_gadget(Canvas& canvas, const Rect& rect,
                              GadgetKind kind, bool active);
    virtual void draw_bottombar(Canvas& canvas, const Rect& rect);
    virtual void draw_resize_gadget(Canvas& canvas, const Rect& rect);
    virtual void draw_window_frame(Canvas& canvas, const Rect& rect,
                                    bool active);
    // The scrollbar (specs/trinket/scrollbar.md): its trough, thumb and two arrow
    // cells, whose rectangles the widget computes. `vertical` names the
    // orientation the arrows point along; each `*_pressed` draws the MUI
    // selected frame while that button is held.
    virtual void draw_scrollbar(Canvas& canvas, const Rect& rect, bool vertical,
                                 const Rect& trough, const Rect& thumb,
                                 const Rect& decrement, const Rect& increment,
                                 bool decrement_pressed, bool increment_pressed);
    // The slider (specs/trinket/slider.md): its trough and its knob, whose
    // geometry the widget computes as the scrollbar's is.
    virtual void draw_slider(Canvas& canvas, const Rect& trough, const Rect& knob,
                             bool hovered);
    // The cycle (specs/trinket/cycle.md): its boxed face, the divider and the
    // mark, whose rectangles the widget computes.
    virtual void draw_cycle(Canvas& canvas, const Rect& rect, const Rect& divider,
                            const Rect& mark, bool pressed, bool hovered);
    // The popup button (specs/trinket/popup_button.md): the MUI image as the
    // whole button, in its normal or selected frame.
    virtual void draw_popup(Canvas& canvas, const Rect& rect,
                            PopupButton::Role role, bool selected);
    // The list (specs/trinket/listview.md): its well, and one row in its state
    // -- the row being pointed at is the dither and the chosen row a solid
    // selection (theme-xen.md).
    enum class ListRow { NORMAL, CURSOR, SELECTED };
    virtual void draw_list(Canvas& canvas, const Rect& rect);
    virtual void draw_list_row(Canvas& canvas, const Rect& rect, ListRow state);
    // A row's image (specs/trinket/listview.md, file_requester.md): the imported
    // MUI drawer/volume artwork, scaled into the rect. Icon::NONE draws nothing.
    virtual void draw_icon(Canvas& canvas, const Rect& rect, Icon icon);
    // A tab (specs/trinket/tabs.md): its chamfered raised face and its title,
    // whose rectangle the widget computes. `active`'s bottom edge is left open
    // where it meets the page body, so the two read as one; `hovered` lightens
    // the face as a button's does.
    virtual void draw_tab(Canvas& canvas, const Rect& rect,
                          std::u32string_view title, bool active, bool hovered);
    // The editor's caret (specs/trinket/editor.md): the cell rectangle is the
    // widget's, the shape is the mode's. BLOCK fills the cell and UNDERLINE
    // draws a line along its foot, both in CURSOR_BG; the widget draws the glyph
    // under a block in CURSOR_TEXT. Insert mode is the block, overwrite the
    // underline.
    enum class CursorShape { BLOCK, UNDERLINE };
    virtual void draw_cursor(Canvas& canvas, const Rect& cell, CursorShape shape);
    virtual void draw_focus_ring(Canvas& canvas, const Rect& rect);
    virtual void draw_tooltip(Canvas& canvas, const Rect& rect,
                               const char* text);

    // XEN/Workbench theme
    static std::unique_ptr<Theme> create_xen(float scale = 1.0f);

    // High contrast theme (accessibility)
    static std::unique_ptr<Theme> create_high_contrast(float scale = 1.0f);
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_THEME_H