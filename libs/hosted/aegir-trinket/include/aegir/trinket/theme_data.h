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
 *   MARK    kind (0 up, 1 down, 2 left, 3 right, 4 circle), color (light),
 *           color2 (dark),
 *           and num/den, the mark's half-extent as a fraction of the smaller
 *           side of the rectangle
 *   SPRITE  index into kSprites, blitted centred
 * Every step also carries `inset`, the number of pixels its rectangle is set in
 * from the gadget's. */
enum class Prim : uint8_t { FILL, BEVEL, OUTLINE, DITHER, MARK, SPRITE };

struct Step {
    Prim op;
    uint8_t kind = 0;
    uint8_t inset = 0;
    uint8_t num = 0;
    uint8_t den = 1;
    uint32_t color = 0;
    uint32_t color2 = 0;
    uint16_t sprite = 0;
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
extern const Recipe kRecipeTextbox[2];  // normal, focused
extern const Recipe kRecipeTextboxReadonly;
extern const Recipe kRecipePanel[5];    // Panel::Style: FLAT, RAISED, SUNKEN, FRAME, GROUP_BOX
extern const Recipe kRecipeScrollbarTrough;
extern const Recipe kRecipeScrollbarThumb;
extern const Recipe kRecipeScrollbarDecrement;
extern const Recipe kRecipeScrollbarIncrement;

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_THEME_DATA_H
