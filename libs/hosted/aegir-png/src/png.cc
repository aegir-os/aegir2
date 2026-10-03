/*
 * The PNG decoder, built (specs/datatypes.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/png.h>

#include <png.h>

#include <cstring>
#include <utility>

namespace aegir::datatypes::png {

bool identify(const uint8_t *data, size_t size) noexcept
{
    /* The signature is eight bytes; png_sig_cmp answers nonzero past, and only
     * the first eight are asked about (libpng's own rule). */
    return data != nullptr && size >= 8 && png_sig_cmp(data, 0, 8) == 0;
}

namespace {

/* The one layout the class serves: RGBA, eight bits a channel. */
Info info_for(png_image const &image)
{
    Info info;
    info.width = image.width;
    info.height = image.height;
    info.format = Format::RGBA;
    info.stride = image.width * 4;
    info.palette_size = 0;
    info.transparent = false;
    info.transparent_index = 0;
    return info;
}

void begin(png_image &image)
{
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
}

} // namespace

bool probe(const uint8_t *data, size_t size, Info &out) noexcept
{
    png_image image;
    begin(image);
    if (png_image_begin_read_from_memory(&image, data, size) == 0) {
        return false;
    }
    out = info_for(image);
    png_image_free(&image);
    return true;
}

bool decode(const uint8_t *data, size_t size, Decoded &out)
{
    png_image image;
    begin(image);
    if (png_image_begin_read_from_memory(&image, data, size) == 0) {
        return false;
    }

    /* The colour type is normalised here: palette, alpha and greyscale all
     * arrive as RGBA, which is what the client draws. The format decides the
     * row stride, so it is set before the buffer is sized. */
    image.format = PNG_FORMAT_RGBA;

    Decoded decoded;
    decoded.info = info_for(image);
    decoded.pixels.resize(PNG_IMAGE_SIZE(image));

    if (png_image_finish_read(&image, nullptr, decoded.pixels.data(), 0, nullptr) == 0) {
        png_image_free(&image);
        return false;
    }
    png_image_free(&image);
    decoded.palette.clear();

    out = std::move(decoded);
    return true;
}

} // namespace aegir::datatypes::png
