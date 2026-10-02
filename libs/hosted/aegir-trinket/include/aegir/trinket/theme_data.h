/*
 * Trinket theme data: the recipe a gadget's look is made of.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The look is data (specs/trinket/theming.md): a gadget is an ordered list of
 * primitive steps -- fill, bevel, outline, dither, mark -- and the theme
 * interprets them. The tables are generated from resources/themes/xen.toml by
 * scripts/gen_theme.py; the theme file is the source, this is its shape.
 */

#ifndef AEGIR_TRINKET_THEME_DATA_H
#define AEGIR_TRINKET_THEME_DATA_H

#include <cstdint>

namespace aegir::trinket {

/* One drawing primitive in a recipe. Which fields matter depends on `op`:
 *   FILL    color
 *   BEVEL   kind (0 raised, 1 sunken)
 *   OUTLINE color
 *   DITHER  color (foreground), color2 (background)
 *   MARK    kind (0 up, 1 down, 2 left, 3 right), color (light), color2 (dark),
 *           and num/den, the mark's half-extent as a fraction of the smaller
 *           side of the rectangle
 *   LINE    kind (0 top, 1 bottom, 2 left, 3 right) and color -- one edge, not
 *           the whole bevel: the title bar's two lines and the menu bar's
 *           underline (specs/trinket/chrome.md)
 *   SPRITE  index into kSprites, blitted centred
 * Every step draws into `inset` pixels in from the gadget's rectangle, then --
 * when x0/y0/x1/y1 name a sub-rectangle -- into the sixteenths of that: the box
 * a gadget's glyph sits in, which a uniform inset cannot offset or nest
 * (specs/trinket/chrome.md). The default is the whole rectangle. */
enum class Prim : uint8_t { FILL, BEVEL, OUTLINE, DITHER, MARK, LINE, SPRITE };

struct Step {
    Prim op;
    uint8_t kind = 0;
    uint8_t inset = 0;
    uint8_t num = 0;
    uint8_t den = 1;
    uint32_t color = 0;
    uint32_t color2 = 0;
    uint16_t sprite = 0;
    /* The sub-rectangle, in sixteenths of the (inset) rectangle; the default is
     * the whole of it. */
    uint8_t x0 = 0;
    uint8_t y0 = 0;
    uint8_t x1 = 16;
    uint8_t y1 = 16;
};

/* A bitmap sprite: the theme's imported art (specs/trinket/theming.md). Pixels
 * are 0xAARRGGBB, top-left first; alpha 0 is transparent. */
struct Sprite {
    uint16_t width;
    uint16_t height;
    const uint32_t* pixels;
};

struct Recipe {
    const Step* steps;
    uint16_t count;
};

/* Generated from resources/themes/xen.toml -- see scripts/gen_theme.py. */
extern const uint32_t kPalette[];      // indexed by ColorRole
extern const int kMetrics[];           // indexed by MetricRole, at 96 dpi
extern const uint8_t kMetricFixed[];   // 1 when a metric is not scaled
extern const Sprite kSprites[];        // the imported artwork, by index
extern const unsigned kSpriteCount;
extern const Recipe kRecipeButton[5];   // normal, hovered, pressed, focused, disabled
extern const Recipe kRecipeCheck[2];    // unchecked, checked
extern const Recipe kRecipeRadio[2];    // unchecked, checked
extern const Recipe kRecipeTextbox[2];  // normal, focused
extern const Recipe kRecipeTextboxReadonly;
extern const Recipe kRecipePanel[5];    // Panel::Style: FLAT, RAISED, SUNKEN, FRAME, GROUP_BOX
extern const Recipe kRecipeScrollbarTrough;
extern const Recipe kRecipeScrollbarThumb;
// The arrow buttons, by orientation and press state: index pressed ? 1 : 0.
extern const Recipe kRecipeScrollbarDecrement[2];            // vertical
extern const Recipe kRecipeScrollbarIncrement[2];
extern const Recipe kRecipeScrollbarDecrementHorizontal[2];  // horizontal
extern const Recipe kRecipeScrollbarIncrementHorizontal[2];
extern const Recipe kRecipeSliderTrough;
extern const Recipe kRecipeSliderKnob;
extern const Recipe kRecipeCycleFace;
extern const Recipe kRecipeCyclePressed;
extern const Recipe kRecipeCycleDivider;
extern const Recipe kRecipeCycleMark;
// The popup button's three roles (POPUP, FILE, DRAWER), each in a normal and a
// selected frame: index role * 2 + (selected ? 1 : 0).
extern const Recipe kRecipePopup[6];
extern const Recipe kRecipeListWell;
extern const Recipe kRecipeListCursor;
extern const Recipe kRecipeListSelected;
// A row's image, indexed by Icon: NONE, DRAWER, HARD_DISK, DISK, CHIP, VOLUME,
// NETWORK (specs/trinket/listview.md).
extern const Recipe kRecipeIcon[7];
// The window chrome (specs/trinket/chrome.md): the title bar in its state, the
// bottom bar, the window frame, and the close/zoom/depth gadgets (indexed
// kind * 2 + (active ? 1 : 0)).
extern const Recipe kRecipeTitlebar[2];  // inactive, active
extern const Recipe kRecipeBottombar;
extern const Recipe kRecipeWindowFrame;
extern const Recipe kRecipeGadget[6];
// The screen bar and the menus (specs/trinket/chrome.md): the screen bar, the
// in-window menu bar, a popup's well, one row (normal, hovered), and a keycap.
extern const Recipe kRecipeScreenBar;
extern const Recipe kRecipeMenubar;
extern const Recipe kRecipeMenuWell;
extern const Recipe kRecipeMenuItem[2];
extern const Recipe kRecipeKeycap;

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_THEME_DATA_H
