/*
 * Trinket ServerFont implementation (see server_font.h, specs/fonts.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/server_font.h>

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/font.h>
#include <aegir/nmspace.h>
#include <sel4/sel4.h>

#include <cstring>
#include <algorithm>
#include <string>
#include <utility>

namespace aegir::trinket {

namespace {

/* One answer's records fit the kernel's envelope, and the page trims a batch
 * further: a request is capped here and the rest is asked for again. */
constexpr uint32_t kBatch =
    (aegir::ipc::kMaxWords - 2) / aegir::font::kGlyphsWordsPerCode;

}  // namespace

std::unique_ptr<ServerFont> ServerFont::open(aegir::ipc::Consumer service,
                                             aegir::mem::Allocator& allocator,
                                             aegir::mem::Scratch& scratch,
                                             std::string_view family, int size,
                                             bool bold, bool italic) {
    if (!service.valid() || family.empty() || size <= 0) {
        return nullptr;
    }
    auto font = std::unique_ptr<ServerFont>(new ServerFont());
    font->service_ = service;

    /* The page. One frame of the client's own, mapped so the glyphs the service
     * writes can be read out, and a second, *unmapped* capability to hand over:
     * a frame capability pins to the VSpace it is first mapped into
     * (seL4_RISCV_Page_Map refuses one already mapped elsewhere), and
     * seL4_CNode_Copy derives the capability, which clears the mapping
     * (Arch_deriveCap) -- so the copy crosses, and the service can map it. */
    aegir::mem::Account account{"font-page", 0, 0, 0};
    seL4_Error error = seL4_NoError;
    font->frame_ = allocator.alloc_page(account, &error);
    if (font->frame_ == 0) {
        return nullptr;
    }
    font->page_ = static_cast<uint8_t*>(scratch.map(font->frame_));
    if (font->page_ == nullptr) {
        return nullptr;
    }
    font->send_ = allocator.alloc_slot();
    if (font->send_ == 0 ||
        seL4_CNode_Copy(aegir::bootstrap::kSlotOwnCNode, font->send_,
                        aegir::bootstrap::cnode_bits(), aegir::bootstrap::kSlotOwnCNode,
                        font->frame_, aegir::bootstrap::cnode_bits(),
                        seL4_AllRights) != seL4_NoError) {
        return nullptr;
    }

    /* open: the family, the pixel size, the style. */
    uint64_t request[aegir::ipc::kMaxWords] = {};
    uint32_t const words =
        aegir::nmspace::pack_string(request, family.data(),
                                    static_cast<uint32_t>(family.size()),
                                    aegir::nmspace::kPathMax);
    if (words == 0) {
        return nullptr;
    }
    request[words] = static_cast<uint64_t>(size);
    request[words + 1] = bold ? 1 : 0;
    request[words + 2] = italic ? 1 : 0;
    uint64_t answer[8] = {};
    aegir::ipc::WordsReply const reply =
        service.call_words(aegir::font::kMethodOpen, request, words + 3, answer, 8);
    if (reply.error != 0 || reply.count < 2 || answer[0] != 1) {
        return nullptr;
    }
    font->face_id_ = answer[1];

    /* metrics: the id. */
    uint64_t metrics[8] = {};
    aegir::ipc::WordsReply const measured =
        service.call_words(aegir::font::kMethodMetrics, &font->face_id_, 1, metrics, 8);
    if (measured.error != 0 || measured.count < 5 || metrics[0] != 1) {
        return nullptr;
    }
    font->ascent_ = static_cast<int>(metrics[1]);
    font->descent_ = static_cast<int>(metrics[2]);
    font->line_gap_ = static_cast<int>(metrics[3]);
    /* The service's fourth word is the line height the face recommends, and for
     * a face whose line gap is negative (Noto Sans') that is *shorter* than the
     * glyphs it draws -- a line sized to it shaves their descenders. A line
     * height is a floor for what the line must hold, so the greater of the two
     * is what the toolkit lays out with (the same notion BitmapFont's BDF path
     * uses, where there is no line gap to add). */
    font->height_ = std::max(static_cast<int>(metrics[4]), font->ascent_ - font->descent_);
    return font;
}

ServerFont::~ServerFont() {
    if (service_.valid() && face_id_ != 0) {
        static_cast<void>(service_.call(aegir::font::kMethodClose, face_id_));
    }
    /* Deleting the capability unmaps the page: the kernel removes a mapped
     * frame whose cap is deleted (the note Scratch::rewind carries). The
     * scratch's cursor is left where it is -- the page was mapped for the
     * font's whole life, not the map-write-unmap rhythm `Scratch::unmap` is
     * for, so its bookkeeping is not ours to move. */
    if (send_ != 0) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, send_,
                          aegir::bootstrap::cnode_bits());
    }
    if (frame_ != 0) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, frame_,
                          aegir::bootstrap::cnode_bits());
    }
}

bool ServerFont::fetch(std::vector<uint32_t> const& codes, uint32_t* answered) {
    if (codes.empty() || page_ == nullptr || send_ == 0 || face_id_ == 0) {
        return false;
    }
    uint32_t const count =
        codes.size() < kBatch ? static_cast<uint32_t>(codes.size()) : kBatch;
    uint64_t request[aegir::ipc::kMaxWords] = {};
    request[0] = face_id_;
    request[1] = count;
    for (uint32_t i = 0; i < count; ++i) {
        request[2 + i] = codes[i];
    }
    uint64_t answer[aegir::ipc::kMaxWords] = {};
    aegir::ipc::WordsReply const reply = service_.call_transfer(
        aegir::font::kMethodGlyphs, request, 2 + count, send_, answer,
        aegir::ipc::kMaxWords, nullptr);
    if (reply.error != 0 || reply.count < 2 || answer[0] != 1) {
        return false;
    }
    uint32_t const got = static_cast<uint32_t>(answer[1]);
    uint32_t const room = (reply.count - 2) / aegir::font::kGlyphsWordsPerCode;
    uint32_t const have = got < room ? got : room;
    if (have > count) {
        return false;
    }
    for (uint32_t i = 0; i < have; ++i) {
        uint32_t const at = 2 + i * aegir::font::kGlyphsWordsPerCode;
        Glyph glyph;
        glyph.advance = static_cast<int>(answer[at + 0]);
        glyph.bearing_x = static_cast<int>(answer[at + 1]);
        glyph.bearing_y = static_cast<int>(answer[at + 2]);
        glyph.width = static_cast<int>(answer[at + 3]);
        glyph.height = static_cast<int>(answer[at + 4]);
        glyph.valid = answer[at + 6] != 0;
        uint32_t const offset = static_cast<uint32_t>(answer[at + 5]);
        size_t const bytes = static_cast<size_t>(glyph.width) * glyph.height;
        if (glyph.valid && bytes > 0) {
            if (offset > aegir::font::kTransferPageBytes ||
                bytes > aegir::font::kTransferPageBytes - offset) {
                return false;
            }
            if (!adopt_glyph(codes[i], glyph, page_ + offset)) {
                return false;
            }
        } else {
            glyph_map_[codes[i]] = glyph;
        }
    }
    if (answered != nullptr) {
        *answered = have;
    }
    return true;
}

bool ServerFont::drain(std::vector<uint32_t>& missing) {
    while (!missing.empty()) {
        uint32_t answered = 0;
        if (!fetch(missing, &answered) || answered == 0) {
            return false;
        }
        missing.erase(missing.begin(), missing.begin() + answered);
    }
    return true;
}

const Glyph* ServerFont::glyph(uint32_t codepoint) const {
    if (glyph_map_.find(codepoint) == glyph_map_.end()) {
        auto* self = const_cast<ServerFont*>(this);
        std::vector<uint32_t> const one{codepoint};
        uint32_t answered = 0;
        bool const ok = self->fetch(one, &answered);
        if (!ok && !self->reported_) {
            self->reported_ = true;
            aegir::debug_write("  trinket: FAIL the font service would not answer glyphs\n");
        }
        if (!ok || answered == 0) {
            /* Remember the miss: a draw must not become one call per glyph per
             * frame, and an absent codepoint is the fallback chain's business
             * (specs/fonts.md). */
            self->glyph_map_[codepoint] = Glyph{};
        }
    }
    return BitmapFont::glyph(codepoint);
}

Size ServerFont::measure(std::u32string_view text) const {
    auto* self = const_cast<ServerFont*>(this);
    std::vector<uint32_t> missing;
    for (char32_t const cp : text) {
        auto const code = static_cast<uint32_t>(cp);
        if (glyph_map_.find(code) != glyph_map_.end()) {
            continue;
        }
        bool seen = false;
        for (uint32_t const already : missing) {
            if (already == code) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            missing.push_back(code);
        }
        if (missing.size() >= kBatch) {
            static_cast<void>(self->drain(missing));
        }
    }
    if (!missing.empty()) {
        static_cast<void>(self->drain(missing));
    }
    return Font::measure(text);
}

}  // namespace aegir::trinket
