/*
 * Trinket color types implementation.
 */

#include <aegir/trinket/color.h>

namespace aegir::trinket {

Color Color::blend(Color src, Color dst) {
    // Premultiplied alpha over operator
    int src_a = src.a;
    int dst_a = dst.a;
    int out_a = src_a + ((255 - src_a) * dst_a) / 255;
    if (out_a == 0) return {0, 0, 0, 0};

    return {
        static_cast<uint8_t>((src.r * 255 + dst.r * (255 - src_a)) / 255 * 255 / out_a),
        static_cast<uint8_t>((src.g * 255 + dst.g * (255 - src_a)) / 255 * 255 / out_a),
        static_cast<uint8_t>((src.b * 255 + dst.b * (255 - src_a)) / 255 * 255 / out_a),
        static_cast<uint8_t>(out_a)
    };
}

Color Color::lerp(Color a, Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {
        static_cast<uint8_t>(a.r + (b.r - a.r) * t + 0.5f),
        static_cast<uint8_t>(a.g + (b.g - a.g) * t + 0.5f),
        static_cast<uint8_t>(a.b + (b.b - a.b) * t + 0.5f),
        static_cast<uint8_t>(a.a + (b.a - a.a) * t + 0.5f)
    };
}

} // namespace aegir::trinket