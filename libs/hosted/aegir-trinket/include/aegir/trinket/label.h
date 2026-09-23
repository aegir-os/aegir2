/*
 * Trinket Label widget.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_LABEL_H
#define AEGIR_TRINKET_LABEL_H

#include <aegir/trinket/widget.h>
#include <aegir/trinket/font.h>
#include <aegir/trinket/unicode.h>

namespace aegir::trinket {

class Label : public Widget {
public:
    enum class Alignment { LEFT, CENTER, RIGHT };
    enum class WordWrap { NONE, CHARACTER, WORD };

    Label(std::u32string_view text = U"");
    Label(std::string_view text);
    ~Label() override;

    void set_text(std::u32string_view text);
    void set_text(std::string_view text);  // UTF-8
    const std::u32string& text() const { return text_; }

    void set_alignment(Alignment a) { alignment_ = a; damage(); }
    void set_word_wrap(WordWrap w) { word_wrap_ = w; damage(); }
    void set_font(Font* f) { font_ = f; damage(); }
    Font* font() const { return font_; }

    void set_text_color(Color c) { text_color_ = c; damage(); }
    Color text_color() const { return text_color_; }

    void set_ellipsis(bool e) { ellipsis_ = e; damage(); }

protected:
    void on_paint(Canvas& canvas, const PaintEvent& event) override;
    Size preferred_size() const;

private:
    std::u32string text_;
    Alignment alignment_ = Alignment::LEFT;
    WordWrap word_wrap_ = WordWrap::NONE;
    Font* font_ = nullptr;
    Color text_color_ = Color::BLACK;
    bool ellipsis_ = false;
    mutable std::u32string elided_text_;
    mutable bool text_dirty_ = true;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_LABEL_H