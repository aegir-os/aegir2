/*
 * Host conformance for the text document (specs/trinket/editor.md).
 *
 * The editor's model is pure arithmetic over its lines -- where the caret moves,
 * where an insert and an overwrite land, and which lines a backspace or a delete
 * joins -- so a host check pins it, including the cases a screen shows only by
 * luck: the head and the tail of a line, a line with less room than the column
 * the caret wants, and a document of one empty line.
 *
 * Host tools only (python3 and a C++ compiler); not part of any target build.
 * Run through scripts/check_text_document.py.
 */

#include <aegir/trinket/text_document.h>

#include <cstdio>
#include <string>

namespace {

using aegir::trinket::TextDocument;

unsigned g_checks = 0;
unsigned g_failures = 0;

void check(bool ok, std::string const& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

/* The driver's own text is ASCII; a non-ASCII codepoint is echoed as '?' so a
 * mismatch still reads, and its own case checks the caret counts codepoints. */
std::string render(std::u32string_view text) {
    std::string out;
    for (char32_t const cp : text) {
        out.push_back(cp < 0x80 ? static_cast<char>(cp) : '?');
    }
    return out;
}

void expect_text(TextDocument const& doc, std::string const& want,
                 std::string const& what) {
    std::string const got = render(doc.text());
    check(got == want, what + ": got '" + got + "', want '" + want + "'");
}

void expect_cursor(TextDocument const& doc, int line, int column,
                   std::string const& what) {
    bool const ok = doc.cursor().line == line && doc.cursor().column == column;
    check(ok, what + ": got " + std::to_string(doc.cursor().line) + "," +
                  std::to_string(doc.cursor().column) + ", want " +
                  std::to_string(line) + "," + std::to_string(column));
}

void type(TextDocument& doc, char const* text) {
    for (char const* p = text; *p != '\0'; ++p) {
        if (*p == '\n') {
            doc.newline();
        } else {
            doc.insert(static_cast<char32_t>(*p));
        }
    }
}

} // namespace

int main() {
    /* The lines split on '\n' and join back; an empty line is a row. */
    {
        TextDocument doc(U"one\ntwo\n\nfour");
        check(doc.line_count() == 4, "four rows from three newlines");
        expect_text(doc, "one\ntwo\n\nfour", "the text round-trips");
        check(doc.line_length(2) == 0, "an empty line is a row");
        check(doc.line_length(1) == 3, "a line's length is its codepoints");
    }
    {
        TextDocument doc;
        check(doc.line_count() == 1, "a fresh document has one line");
        expect_text(doc, "", "a fresh document is empty");
        expect_cursor(doc, 0, 0, "the caret starts at the head");
        check(!doc.dirty(), "a fresh document is clean");
    }

    /* Insert shifts the tail; overwrite replaces the cell under the caret. */
    {
        TextDocument doc;
        type(doc, "abc");
        expect_text(doc, "abc", "typing inserts");
        expect_cursor(doc, 0, 3, "the caret follows the typing");
        check(doc.dirty(), "an edit marks the document dirty");
        doc.clear_dirty();
        check(!doc.dirty(), "clear_dirty clears it");
    }
    {
        TextDocument doc(U"abcd");
        doc.set_mode(TextDocument::Mode::OVERWRITE);
        type(doc, "XY");
        expect_text(doc, "XYcd", "overwrite replaces the cells");
        expect_cursor(doc, 0, 2, "and the caret keeps its step");
    }
    {
        TextDocument doc(U"ab");
        doc.set_mode(TextDocument::Mode::OVERWRITE);
        doc.move(0, 0);
        doc.end();
        type(doc, "c");
        expect_text(doc, "abc", "overwrite appends at a line's end");
    }

    /* A newline splits the line at the caret. */
    {
        TextDocument doc(U"abcd");
        doc.set_cursor(0, 2);
        doc.newline();
        expect_text(doc, "ab\ncd", "a newline splits the line");
        expect_cursor(doc, 1, 0, "the caret is at the new line's head");
    }

    /* Backspace is the cell before the caret, joining at a line's head. */
    {
        TextDocument doc(U"abc");
        doc.set_cursor(0, 2);
        doc.backspace();
        expect_text(doc, "ac", "backspace erases the cell before");
        expect_cursor(doc, 0, 1, "and the caret steps back");
    }
    {
        TextDocument doc(U"ab\ncd");
        doc.set_cursor(1, 0);
        doc.backspace();
        expect_text(doc, "abcd", "backspace at a line's head joins it up");
        expect_cursor(doc, 0, 2, "the caret lands where the lines met");
    }
    {
        TextDocument doc(U"ab");
        doc.set_cursor(0, 0);
        doc.backspace();
        expect_text(doc, "ab", "backspace at the very head does nothing");
        expect_cursor(doc, 0, 0, "and the caret stays");
    }

    /* Delete is the cell under the caret, joining at a line's end. */
    {
        TextDocument doc(U"abc");
        doc.set_cursor(0, 1);
        doc.erase();
        expect_text(doc, "ac", "delete erases the cell under");
        expect_cursor(doc, 0, 1, "and the caret stays");
    }
    {
        TextDocument doc(U"ab\ncd");
        doc.set_cursor(0, 2);
        doc.erase();
        expect_text(doc, "abcd", "delete at a line's end joins it down");
        expect_cursor(doc, 0, 2, "the caret stays at the join");
    }
    {
        TextDocument doc(U"ab");
        doc.end();
        doc.erase();
        expect_text(doc, "ab", "delete at the very end does nothing");
    }

    /* Horizontal movement clamps to the line. */
    {
        TextDocument doc(U"abc");
        doc.move(-1, 0);
        expect_cursor(doc, 0, 0, "left at the head stays");
        doc.move(5, 0);
        expect_cursor(doc, 0, 3, "right past the end clamps");
        doc.move(-1, 0);
        expect_cursor(doc, 0, 2, "and one back is one cell");
    }

    /* Vertical movement aims for the column the caret last held on its own. */
    {
        TextDocument doc(U"abcde\nx\nfghij");
        doc.set_cursor(0, 4);
        doc.move(0, 1);
        expect_cursor(doc, 1, 1, "down onto a short line clamps the column");
        doc.move(0, 1);
        expect_cursor(doc, 2, 4, "and down again returns to the wanted column");
        doc.move(0, -1);
        expect_cursor(doc, 1, 1, "up onto the short line clamps again");
        doc.move(0, -1);
        expect_cursor(doc, 0, 4, "and up again returns it");
        doc.move(0, -1);
        expect_cursor(doc, 0, 4, "up at the first line stays");
        doc.set_cursor(2, 0);
        doc.move(0, 5);
        expect_cursor(doc, 2, 0, "down past the last line clamps");
    }

    /* A horizontal step is the caret's own, and clears the wanted column. */
    {
        TextDocument doc(U"abcde\nx");
        doc.set_cursor(0, 4);
        doc.move(-2, 0);
        expect_cursor(doc, 0, 2, "a step left is two cells");
        doc.move(0, 1);
        expect_cursor(doc, 1, 1, "down now aims for the stepped column");
    }

    /* Home and End, and a set that clamps. */
    {
        TextDocument doc(U"hello");
        doc.end();
        expect_cursor(doc, 0, 5, "end is the line's length");
        doc.home();
        expect_cursor(doc, 0, 0, "home is the line's head");
        doc.set_cursor(9, 9);
        expect_cursor(doc, 0, 5, "a cursor past the end clamps");
        doc.set_cursor(-1, -1);
        expect_cursor(doc, 0, 0, "a cursor before the head clamps");
    }

    /* A line's length counts codepoints, not bytes: a wide glyph is one caret
     * step and one cell of the model. */
    {
        TextDocument doc;
        doc.insert(0x65E5); /* U+65E5, the CJK day */
        doc.insert(0x672C); /* U+672C */
        check(doc.line_length(0) == 2, "two codepoints are two cells");
        expect_cursor(doc, 0, 2, "and the caret is after both");
    }

    std::printf("text document: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
