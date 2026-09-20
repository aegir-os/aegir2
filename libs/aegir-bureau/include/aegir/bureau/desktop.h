/*
 * Bureau Desktop - stub for Phase 2.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_BUREAU_DESKTOP_H
#define AEGIR_BUREAU_DESKTOP_H

#include <aegir/trinket/point.h>
#include <aegir/trinket/unicode.h>
#include <cstdint>
#include <vector>

namespace aegir::bureau::desktop {

// Desktop icons (Phase 2)
struct Icon {
    uint64_t id;
    std::u32string label;
    std::u32string target;  // App binary or file
    aegir::trinket::Point pos;
};

// Desktop backdrop (Workbench-style)
class Backdrop {
public:
    void set_color(uint32_t color);
    void set_pattern(std::u32string_view pattern_name);
    void set_image(std::u32string_view path);
};

} // namespace aegir::bureau::desktop

#endif // AEGIR_BUREAU_DESKTOP_H