/*
 * A decoded image a datatype class holds while a caller pulls it out.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A message carries one capability (specs/launch.md, specs/signal.md), so a
 * picture larger than a page cannot cross in one `read`; the class decodes once
 * into this buffer and serves it a page at a time (specs/datatypes.md). The
 * frame stream is the pixels then the palette, and `Info` alone says how long
 * each is -- `stride * height` bytes, then `palette_size` colours of three
 * bytes -- so a caller never guesses.
 */

#ifndef AEGIR_DATATYPE_DECODED_H
#define AEGIR_DATATYPE_DECODED_H

#include <aegir/datatypes.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aegir::datatypes {

struct Decoded {
    Info info;
    std::vector<uint8_t> pixels;
    std::vector<Color> palette;

    /* The whole frame the class serves: pixels then palette. */
    std::size_t stream_size() const noexcept
    {
        return pixels.size() + palette.size() * 3;
    }

    /* Copy up to `len` bytes of the stream at `offset`, in the frame order
     * above. Returns how many were written; a short read is the end of the
     * image, not an error, and the caller detects it by the count. */
    std::size_t read(uint64_t offset, uint8_t *out, std::size_t len) const noexcept
    {
        std::size_t written = 0;
        while (written < len) {
            const uint64_t at = offset + written;
            if (at < pixels.size()) {
                out[written] = pixels[static_cast<std::size_t>(at)];
            } else {
                const uint64_t p = at - pixels.size();
                if (p >= palette.size() * 3) break;
                const Color &c = palette[static_cast<std::size_t>(p / 3)];
                const std::size_t channel = static_cast<std::size_t>(p % 3);
                out[written] = channel == 0 ? c.r : channel == 1 ? c.g : c.b;
            }
            ++written;
        }
        return written;
    }
};

} // namespace aegir::datatypes

#endif // AEGIR_DATATYPE_DECODED_H
