/*
 * The JPEG decoder, built (specs/datatypes.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/jpeg.h>

/* jpeglib.h names FILE in its stdio entry points without including <stdio.h>
 * itself, so the definition has to be in scope before it is read. */
#include <cstdio>

#include <jpeglib.h>

#include <csetjmp>
#include <cstdlib>
#include <utility>

namespace aegir::datatypes::jpeg {

bool identify(const uint8_t *data, size_t size) noexcept
{
    /* Every JPEG opens with SOI, 0xFFD8; two bytes is what every reader agrees
     * on, and the marker's own 0xFF follows. */
    return data != nullptr && size >= 2 && data[0] == 0xff && data[1] == 0xd8;
}

namespace {

/* libjpeg-turbo's error manager carries the jump target its `error_exit` must
 * land on; longjmp is the API's contract, not a choice. */
struct ErrorManager {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

void on_error(j_common_ptr cinfo)
{
    auto *err = reinterpret_cast<ErrorManager *>(cinfo->err);
    longjmp(err->jump, 1);
}

/* Warnings and notices are swallowed: a class service's stderr is the boot
 * log, and a recoverable decode note is not worth a line there. */
void on_message(j_common_ptr)
{
}

/* The decode's whole state lives on the heap, and its address never changes
 * between setjmp and longjmp, so it stays valid when libjpeg's error_exit
 * jumps back -- the C++ automatic-storage rule that makes a stack `cinfo`
 * indeterminate after a longjmp cannot bite here. */
struct DecodeState {
    jpeg_decompress_struct cinfo;
    ErrorManager err;
    uint8_t *pixels;
};

DecodeState *begin_state()
{
    auto *state = static_cast<DecodeState *>(std::calloc(1, sizeof(DecodeState)));
    if (state == nullptr) {
        return nullptr;
    }
    state->cinfo.err = jpeg_std_error(&state->err.pub);
    state->err.pub.error_exit = on_error;
    state->err.pub.output_message = on_message;
    return state;
}

/* Free a state that may be halfway through a decode: the pixel buffer, the
 * decompressor (a no-op before jpeg_create_decompress), and the state itself. */
void drop_state(DecodeState *state) noexcept
{
    if (state == nullptr) {
        return;
    }
    std::free(state->pixels);
    jpeg_destroy_decompress(&state->cinfo);
    std::free(state);
}

/* The one layout the class serves: RGB, eight bits a channel. */
Info info_for(uint32_t width, uint32_t height)
{
    Info info;
    info.width = width;
    info.height = height;
    info.format = Format::RGB;
    info.stride = width * 3;
    info.palette_size = 0;
    info.transparent = false;
    info.transparent_index = 0;
    return info;
}

} // namespace

bool probe(const uint8_t *data, size_t size, Info &out) noexcept
{
    DecodeState *state = begin_state();
    if (state == nullptr) {
        return false;
    }
    if (setjmp(state->err.jump) != 0) {
        drop_state(state);
        return false;
    }
    jpeg_create_decompress(&state->cinfo);
    jpeg_mem_src(&state->cinfo, data, static_cast<unsigned long>(size));
    if (jpeg_read_header(&state->cinfo, TRUE) != JPEG_HEADER_OK) {
        drop_state(state);
        return false;
    }
    out = info_for(state->cinfo.image_width, state->cinfo.image_height);
    drop_state(state);
    return true;
}

bool decode(const uint8_t *data, size_t size, Decoded &out)
{
    DecodeState *state = begin_state();
    if (state == nullptr) {
        return false;
    }
    if (setjmp(state->err.jump) != 0) {
        drop_state(state);
        return false;
    }
    jpeg_create_decompress(&state->cinfo);
    jpeg_mem_src(&state->cinfo, data, static_cast<unsigned long>(size));
    if (jpeg_read_header(&state->cinfo, TRUE) != JPEG_HEADER_OK) {
        drop_state(state);
        return false;
    }

    /* Normalise the colour type: a greyscale, YCbCr or CMYK JPEG all leave as
     * RGB here, which is what the client draws. */
    state->cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&state->cinfo);

    uint32_t const width = state->cinfo.output_width;
    uint32_t const height = state->cinfo.output_height;
    if (state->cinfo.output_components != 3 || width == 0 || height == 0) {
        drop_state(state);
        return false;
    }
    std::size_t const stride = static_cast<std::size_t>(width) * 3;
    std::size_t const total = stride * height;
    state->pixels = static_cast<uint8_t *>(std::malloc(total));
    if (state->pixels == nullptr) {
        drop_state(state);
        return false;
    }

    /* libjpeg hands back scanlines a row at a time; the buffer is dense, so the
     * row's address is the buffer plus the rows already read. */
    while (state->cinfo.output_scanline < state->cinfo.output_height) {
        JSAMPROW row = state->pixels +
                       static_cast<std::size_t>(state->cinfo.output_scanline) * stride;
        jpeg_read_scanlines(&state->cinfo, &row, 1);
    }

    Decoded decoded;
    decoded.info = info_for(width, height);
    decoded.pixels.assign(state->pixels, state->pixels + total);
    drop_state(state);

    out = std::move(decoded);
    return true;
}

} // namespace aegir::datatypes::jpeg
