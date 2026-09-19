/*
 * Trinket Unicode utilities implementation.
 */

#include <aegir/trinket/unicode.h>

namespace aegir::trinket {

std::u32string utf8_to_utf32(std::string_view utf8) {
    std::u32string result;
    result.reserve(utf8.size());
    for (uint32_t cp : utf8_iterate(utf8)) {
        if (cp != 0) result.push_back(cp);
    }
    return result;
}

std::string utf32_to_utf8(std::u32string_view utf32) {
    std::string result;
    result.reserve(utf32.size() * 3);
    for (char32_t cp : utf32) {
        if (cp < 0x80) {
            result.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return result;
}

bool is_valid_utf8(std::string_view sv) {
    for (uint32_t cp : utf8_iterate(sv)) {
        if (cp == 0xFFFD) return false;
    }
    return true;
}

std::string replace_invalid_utf8(std::string_view sv, char32_t replacement) {
    std::string result;
    result.reserve(sv.size());
    const char* ptr = sv.data();
    const char* end = sv.data() + sv.size();

    while (ptr < end) {
        unsigned char c = *ptr;
        if (c < 0x80) {
            result.push_back(c);
            ptr += 1;
        } else if ((c & 0xE0) == 0xC0 && ptr + 1 < end) {
            uint32_t cp = ((c & 0x1F) << 6) | (ptr[1] & 0x3F);
            if (cp >= 0x80) {
                result.append(ptr, 2);
                ptr += 2;
            } else {
                // Overlong encoding
                result.push_back(static_cast<char>(0xEF));
                result.push_back(static_cast<char>(0xBF));
                result.push_back(static_cast<char>(0xBD));
                ptr += 2;
            }
        } else if ((c & 0xF0) == 0xE0 && ptr + 2 < end) {
            uint32_t cp = ((c & 0x0F) << 12) | ((ptr[1] & 0x3F) << 6) | (ptr[2] & 0x3F);
            if (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) {
                result.append(ptr, 3);
                ptr += 3;
            } else {
                result.push_back(static_cast<char>(0xEF));
                result.push_back(static_cast<char>(0xBF));
                result.push_back(static_cast<char>(0xBD));
                ptr += 3;
            }
        } else if ((c & 0xF8) == 0xF0 && ptr + 3 < end) {
            uint32_t cp = ((c & 0x07) << 18) | ((ptr[1] & 0x3F) << 12) |
                          ((ptr[2] & 0x3F) << 6) | (ptr[3] & 0x3F);
            if (cp >= 0x10000 && cp <= 0x10FFFF) {
                result.append(ptr, 4);
                ptr += 4;
            } else {
                result.push_back(static_cast<char>(0xEF));
                result.push_back(static_cast<char>(0xBF));
                result.push_back(static_cast<char>(0xBD));
                ptr += 4;
            }
        } else {
            // Invalid
            result.push_back(static_cast<char>(0xEF));
            result.push_back(static_cast<char>(0xBF));
            result.push_back(static_cast<char>(0xBD));
            ptr += 1;
        }
    }
    return result;
}

} // namespace aegir::trinket