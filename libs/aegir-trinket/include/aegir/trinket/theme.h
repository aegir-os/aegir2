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
#include <aegir/trinket/panel.h>
#include <memory>

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
    SCROLLBAR_WIDTH, SCROLLBAR_MIN_HANDLE,
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

    // Drawing primitives (can be overridden for custom look)
    virtual void draw_button(Canvas& canvas, const Rect& rect,
                              bool hovered, bool pressed, bool focused,
                              bool checked, bool enabled);
    virtual void draw_panel(Canvas& canvas, const Rect& rect,
                             Panel::Style style, bool focused);
    virtual void draw_textbox(Canvas& canvas, const Rect& rect,
                               bool focused, bool read_only, bool password);
    virtual void draw_menubar(Canvas& canvas, const Rect& rect);
    virtual void draw_menu_item(Canvas& canvas, const Rect& rect,
                                 const char* label, bool hovered,
                                 bool checked, bool disabled, bool separator);
    virtual void draw_titlebar(Canvas& canvas, const Rect& rect,
                                const char* title, bool active);
    virtual void draw_window_frame(Canvas& canvas, const Rect& rect,
                                    bool active);
    virtual void draw_scrollbar(Canvas& canvas, const Rect& rect,
                                 bool vertical, int handle_pos, int handle_size,
                                 bool hovered);
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