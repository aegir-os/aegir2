/*
 * The greeter's face: a minimal 8x8 bitmap font, as string art.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Deliberately small: the greeter's texts are spelled from eighteen glyphs --
 * the lowercase of "name:", "secret:", "log in" and "no such name or secret",
 * plus space and the bullet a secret echoes as. The art is the source (a '#'
 * is a pixel), parsed once at boot into bitmask rows, because a form redraws
 * on every keystroke and sampling strings pixel by pixel is the kind of
 * clever that saves nothing. A glyph that is not here draws as blank: the
 * greeter's own texts are the only ones it ever sets.
 */

#ifndef AEGIR_GREETER_FONT_H
#define AEGIR_GREETER_FONT_H

#include <stdint.h>

namespace greeter {

struct Glyph {
    char ch;
    char const *rows[8];
};

constexpr Glyph kGlyphs[] = {
    {' ', {"........", "........", "........", "........",
           "........", "........", "........", "........"}},
    {':', {"........", "........", "..##....", "..##....",
           "........", "..##....", "..##....", "........"}},
    {'*', {"........", "........", "..####..", "..####..",
           "..####..", "..####..", "........", "........"}},
    {'a', {"........", "........", ".####...", ".....#..",
           ".#####..", "#....#..", "#...##..", ".###.#.."}},
    {'c', {"........", "........", ".#####..", "#.......",
           "#.......", "#.......", ".#####..", "........"}},
    {'d', {".....#..", ".....#..", ".#####..", "#....#..",
           "#....#..", "#....#..", ".#####..", "........"}},
    {'e', {"........", "........", ".####...", "#....#..",
           "######..", "#.......", ".####...", "........"}},
    {'g', {"........", "........", ".#####..", "#....#..",
           "#....#..", ".#####..", ".....#..", "#####..."}},
    {'h', {"#.......", "#.......", "#.###...", "##...#..",
           "#....#..", "#....#..", "#....#..", "........"}},
    {'i', {"..#.....", "........", ".##.....", "..#.....",
           "..#.....", "..#.....", ".###....", "........"}},
    {'l', {".##.....", "..#.....", "..#.....", "..#.....",
           "..#.....", "..#.....", ".###....", "........"}},
    {'m', {"........", "........", "##.##...", "#.#..#..",
           "#.#..#..", "#....#..", "#....#..", "........"}},
    {'n', {"........", "........", "#.###...", "##...#..",
           "#....#..", "#....#..", "#....#..", "........"}},
    {'o', {"........", "........", ".####...", "#....#..",
           "#....#..", "#....#..", ".####...", "........"}},
    {'r', {"........", "........", "#.###...", "##...#..",
           "#.......", "#.......", "#.......", "........"}},
    {'s', {"........", "........", ".#####..", "#.......",
           ".####...", ".....#..", "#####...", "........"}},
    {'t', {"..#.....", "..#.....", "#####...", "..#.....",
           "..#.....", "..#.....", "..###...", "........"}},
    {'u', {"........", "........", "#....#..", "#....#..",
           "#....#..", "#...##..", ".###.#..", "........"}},
};
constexpr uint32_t kGlyphCount = sizeof(kGlyphs) / sizeof(kGlyphs[0]);

/* The parsed rows: one byte a row, bit 7 the leftmost pixel. */
inline uint8_t g_rows[kGlyphCount][8];

inline void font_parse() noexcept
{
    for (uint32_t g = 0; g < kGlyphCount; ++g) {
        for (uint32_t y = 0; y < 8; ++y) {
            uint8_t bits = 0;
            for (uint32_t x = 0; x < 8; ++x) {
                if (kGlyphs[g].rows[y][x] == '#') {
                    bits |= static_cast<uint8_t>(0x80u >> x);
                }
            }
            g_rows[g][y] = bits;
        }
    }
}

/* One character, pixels on, top-left at (x, y). A character the font does
 * not have draws nothing. */
inline void draw_char(uint32_t *pixels, uint64_t stride, uint64_t x, uint64_t y,
                      char ch, uint32_t colour) noexcept
{
    uint32_t g = 0;
    while (g < kGlyphCount && kGlyphs[g].ch != ch) {
        ++g;
    }
    if (g == kGlyphCount) {
        return;
    }
    for (uint64_t row = 0; row < 8; ++row) {
        uint8_t const bits = g_rows[g][row];
        for (uint64_t col = 0; col < 8; ++col) {
            if ((bits & (0x80u >> col)) != 0) {
                pixels[(y + row) * stride + x + col] = colour;
            }
        }
    }
}

inline void draw_text(uint32_t *pixels, uint64_t stride, uint64_t x, uint64_t y,
                      char const *text, uint32_t length, uint32_t colour) noexcept
{
    for (uint32_t i = 0; i < length; ++i) {
        draw_char(pixels, stride, x + static_cast<uint64_t>(i) * 8, y, text[i],
                  colour);
    }
}

}  // namespace greeter

#endif  // AEGIR_GREETER_FONT_H
