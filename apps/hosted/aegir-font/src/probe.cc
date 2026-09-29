/*
 * Reading a face's own name without a rasterizer (probe.h, specs/fonts.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include "probe.h"

#include <string.h>

namespace aegir::font {

namespace {

/* A BDF's properties are a few hundred bytes at the top of the file; this is
 * the header read, not a limit on the face. */
constexpr uint64_t kHeaderRead = 4096;

uint16_t be16(uint8_t const *p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t be32(uint8_t const *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/* A BDF property's value: the leading spaces and the quotes are not part of it
 * (`FAMILY_NAME "Terminus"`). */
std::string bdf_value(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    return std::string(text);
}

bool bdf_int(std::string_view text, int *value)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    if (text.empty() || text.front() < '0' || text.front() > '9') {
        return false;
    }
    int parsed = 0;
    for (char const c : text) {
        if (c < '0' || c > '9') {
            break;
        }
        parsed = parsed * 10 + (c - '0');
    }
    *value = parsed;
    return true;
}

/* A range of the file, or false when it is empty or outside it. */
bool read_range(Reader const &reader, uint64_t offset, uint64_t length,
                std::vector<uint8_t> *out)
{
    if (reader.at == nullptr || length == 0 || offset > reader.size ||
        length > reader.size - offset) {
        return false;
    }
    out->assign(static_cast<size_t>(length), 0);
    return reader.at(reader.context, offset, length, out->data());
}

/* The `name` table's records we care about, preferring an English Windows one
 * over any other, which is where the family and subfamily read cleanest. */
struct NamePick {
    int score = -1;
    std::string value;
};

void take_name(NamePick *pick, int score, std::string value)
{
    if (score > pick->score) {
        pick->score = score;
        pick->value = std::move(value);
    }
}

/* Decode a name record: UTF-16BE on the Unicode and Windows platforms, and
 * Latin-1 elsewhere. to UTF-8, which is what a family name is compared as. */
std::string decode_name(uint8_t const *bytes, uint16_t length, uint16_t platform)
{
    std::string out;
    if (platform == 1) {
        out.assign(reinterpret_cast<char const *>(bytes), length);
        return out;
    }
    for (uint16_t i = 0; i + 1 < length; i += 2) {
        uint32_t const code = (bytes[i] << 8) | bytes[i + 1];
        if (code == 0) {
            continue;
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

bool parse_name_table(std::vector<uint8_t> const &table, std::string *family,
                      std::string *style)
{
    if (table.size() < 6) {
        return false;
    }
    uint16_t const count = be16(&table[2]);
    uint16_t const strings = be16(&table[4]);
    NamePick family_pick;
    NamePick style_pick;
    for (uint16_t i = 0; i < count; ++i) {
        size_t const record = 6 + 12ull * i;
        if (record + 12 > table.size()) {
            break;
        }
        uint16_t const platform = be16(&table[record]);
        uint16_t const encoding = be16(&table[record + 2]);
        uint16_t const language = be16(&table[record + 4]);
        uint16_t const name_id = be16(&table[record + 6]);
        uint16_t const length = be16(&table[record + 8]);
        uint16_t const offset = be16(&table[record + 10]);
        if (name_id != 1 && name_id != 2) {
            continue;
        }
        int score = -1;
        if (platform == 3 && encoding == 1 && language == 0x409) {
            score = 3;
        } else if (platform == 3 && encoding == 1) {
            score = 2;
        } else if (platform == 0 || (platform == 1 && encoding == 0)) {
            score = 1;
        }
        if (score < 0) {
            continue;
        }
        size_t const at = static_cast<size_t>(strings) + offset;
        if (at > table.size() || length > table.size() - at) {
            continue;
        }
        std::string const value = decode_name(&table[at], length, platform);
        if (value.empty()) {
            continue;
        }
        take_name(name_id == 1 ? &family_pick : &style_pick, score, value);
    }
    *family = family_pick.value;
    *style = style_pick.value;
    return true;
}

/* The `name` table's offset and length from the table directory at
 * `directory`, or false when the file has none. */
bool find_name_table(Reader const &reader, uint64_t directory, uint64_t *offset,
                     uint64_t *length)
{
    std::vector<uint8_t> header;
    if (!read_range(reader, directory, 12, &header)) {
        return false;
    }
    uint64_t const count = be16(&header[4]);
    uint64_t const bytes = 12 + 16ull * count;
    std::vector<uint8_t> table;
    if (!read_range(reader, directory, bytes, &table)) {
        return false;
    }
    for (uint64_t i = 0; i < count; ++i) {
        size_t const record = 12 + 16ull * i;
        if (memcmp(&table[record], "name", 4) != 0) {
            continue;
        }
        uint64_t const at = be32(&table[record + 8]);
        uint64_t const size = be32(&table[record + 12]);
        if (at > reader.size || size > reader.size - at) {
            return false;
        }
        *offset = at;
        *length = size;
        return size != 0;
    }
    return false;
}

void add_face(Reader const &reader, uint64_t directory, uint32_t face_index,
              std::vector<FaceInfo> *faces)
{
    uint64_t offset = 0;
    uint64_t length = 0;
    if (!find_name_table(reader, directory, &offset, &length)) {
        return;
    }
    std::vector<uint8_t> table;
    if (!read_range(reader, offset, length, &table)) {
        return;
    }
    std::string family;
    std::string style;
    if (!parse_name_table(table, &family, &style) || family.empty()) {
        return;
    }
    FaceInfo info;
    info.family = std::move(family);
    info.style = std::move(style);
    info.scalable = true;
    info.index = face_index;
    faces->push_back(std::move(info));
}

}  // namespace

bool probe_bdf(std::string_view header, FaceInfo *out)
{
    if (out == nullptr) {
        return false;
    }
    *out = FaceInfo{};
    out->scalable = false;
    std::string weight;
    std::string slant;
    bool named = false;
    size_t at = 0;
    while (at < header.size()) {
        size_t const eol = header.find('\n', at);
        std::string_view line =
            header.substr(at, eol == std::string_view::npos ? std::string_view::npos : eol - at);
        at = eol == std::string_view::npos ? header.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (starts_with(line, "FAMILY_NAME")) {
            out->family = bdf_value(line.substr(11));
            named = !out->family.empty();
        } else if (starts_with(line, "WEIGHT_NAME")) {
            weight = bdf_value(line.substr(11));
        } else if (starts_with(line, "SLANT")) {
            slant = bdf_value(line.substr(5));
        } else if (starts_with(line, "PIXEL_SIZE")) {
            std::string_view rest = line.substr(10);
            int value = 0;
            if (bdf_int(rest, &value)) {
                out->pixel_size = value;
            }
        } else if (starts_with(line, "ENDPROPERTIES") || starts_with(line, "CHARS")) {
            break;
        }
    }
    /* The style a request matches against is the weight and the slant together:
     * a BDF says "Medium" and "I" where an OpenType face says "Medium Italic". */
    out->style = weight;
    if (slant == "I") {
        out->style += " Italic";
    } else if (slant == "O") {
        out->style += " Oblique";
    }
    return named;
}

std::vector<FaceInfo> probe_file(Reader const &reader)
{
    std::vector<FaceInfo> faces;
    std::vector<uint8_t> head;
    if (!read_range(reader, 0, 12, &head)) {
        return faces;
    }
    if (memcmp(head.data(), "STARTFONT", 9) == 0) {
        std::vector<uint8_t> prefix;
        uint64_t const bytes = reader.size < kHeaderRead ? reader.size : kHeaderRead;
        if (read_range(reader, 0, bytes, &prefix)) {
            FaceInfo info;
            if (probe_bdf(std::string_view(reinterpret_cast<char const *>(prefix.data()),
                                           prefix.size()),
                          &info)) {
                faces.push_back(std::move(info));
            }
        }
        return faces;
    }
    if (memcmp(head.data(), "ttcf", 4) == 0) {
        uint64_t const count = be32(&head[8]);
        uint64_t const bytes = 12 + 4ull * count;
        std::vector<uint8_t> table;
        if (!read_range(reader, 0, bytes, &table)) {
            return faces;
        }
        for (uint64_t i = 0; i < count; ++i) {
            add_face(reader, be32(&table[12 + 4 * i]), static_cast<uint32_t>(i), &faces);
        }
        return faces;
    }
    static char const kTrueType[] = "\x00\x01\x00\x00";
    if (memcmp(head.data(), kTrueType, 4) == 0 || memcmp(head.data(), "OTTO", 4) == 0 ||
        memcmp(head.data(), "true", 4) == 0) {
        add_face(reader, 0, 0, &faces);
    }
    return faces;
}

}  // namespace aegir::font
