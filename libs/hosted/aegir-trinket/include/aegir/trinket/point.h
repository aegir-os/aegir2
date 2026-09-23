/*
 * Trinket geometry types.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * All coordinates and sizes are in LOGICAL PIXELS.
 * Physical pixels = logical * scale_factor (from DisplayInfo).
 */

#ifndef AEGIR_TRINKET_POINT_H
#define AEGIR_TRINKET_POINT_H

#include <cstdint>
#include <algorithm>

namespace aegir::trinket {

struct Point {
    int x = 0;
    int y = 0;

    constexpr Point() = default;
    constexpr Point(int x_, int y_) : x(x_), y(y_) {}

    constexpr Point operator+(const Point& other) const { return {x + other.x, y + other.y}; }
    constexpr Point operator-(const Point& other) const { return {x - other.x, y - other.y}; }
    constexpr Point& operator+=(const Point& other) { x += other.x; y += other.y; return *this; }
    constexpr Point& operator-=(const Point& other) { x -= other.x; y -= other.y; return *this; }
    constexpr bool operator==(const Point& other) const { return x == other.x && y == other.y; }
    constexpr bool operator!=(const Point& other) const { return !(*this == other); }
};

struct Size {
    int width = 0;
    int height = 0;

    constexpr Size() = default;
    constexpr Size(int w, int h) : width(w), height(h) {}

    constexpr bool empty() const { return width <= 0 || height <= 0; }
    constexpr bool operator==(const Size& other) const { return width == other.width && height == other.height; }
    constexpr bool operator!=(const Size& other) const { return !(*this == other); }
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    constexpr Rect() = default;
    constexpr Rect(int x_, int y_, int w, int h) : x(x_), y(y_), width(w), height(h) {}
    constexpr Rect(Point p, Size s) : x(p.x), y(p.y), width(s.width), height(s.height) {}

    constexpr Point origin() const { return {x, y}; }
    constexpr Size size() const { return {width, height}; }
    constexpr int left() const { return x; }
    constexpr int top() const { return y; }
    constexpr int right() const { return x + width; }
    constexpr int bottom() const { return y + height; }
    constexpr Point center() const { return {x + width / 2, y + height / 2}; }

    constexpr bool empty() const { return width <= 0 || height <= 0; }
    constexpr bool contains(Point p) const {
        return p.x >= x && p.x < x + width && p.y >= y && p.y < y + height;
    }
    constexpr bool intersects(const Rect& other) const {
        return x < other.x + other.width && x + width > other.x &&
               y < other.y + other.height && y + height > other.y;
    }
    constexpr Rect intersected(const Rect& other) const {
        int l = std::max(x, other.x);
        int t = std::max(y, other.y);
        int r = std::min(x + width, other.x + other.width);
        int b = std::min(y + height, other.y + other.height);
        if (l >= r || t >= b) return {};
        return {l, t, r - l, b - t};
    }
    constexpr Rect united(const Rect& other) const {
        if (empty()) return other;
        if (other.empty()) return *this;
        int l = std::min(x, other.x);
        int t = std::min(y, other.y);
        int r = std::max(x + width, other.x + other.width);
        int b = std::max(y + height, other.y + other.height);
        return {l, t, r - l, b - t};
    }
    constexpr Rect translated(Point p) const { return {x + p.x, y + p.y, width, height}; }
    constexpr Rect& translate(Point p) { x += p.x; y += p.y; return *this; }
    constexpr Rect inflated(int dx, int dy) const { return {x - dx, y - dy, width + 2*dx, height + 2*dy}; }
    constexpr Rect inflated(int d) const { return inflated(d, d); }

    constexpr bool operator==(const Rect& other) const {
        return x == other.x && y == other.y && width == other.width && height == other.height;
    }
    constexpr bool operator!=(const Rect& other) const { return !(*this == other); }
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_POINT_H