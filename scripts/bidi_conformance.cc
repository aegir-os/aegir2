/*
 * The UAX #9 conformance driver (specs/locale.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A host program, not built into any target: scripts/check_bidi.py compiles it
 * against the toolkit's bidi.cc, the generated tables, and the pinned Unicode
 * test files, and runs it in either mode. "classes" reads BidiTest.txt (a
 * sequence of bidi classes per case); "chars" reads BidiCharacterTest.txt
 * (code points, paragraph direction, expected levels and order).
 */
#include <aegir/trinket/bidi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using aegir::trinket::BidiClass;
using aegir::trinket::BidiDirection;

static int g_failures = 0;
static int g_checks = 0;

static const std::pair<char const *, BidiClass> kNames[] = {
    {"L", BidiClass::L},       {"R", BidiClass::R},     {"AL", BidiClass::AL},
    {"EN", BidiClass::EN},     {"ES", BidiClass::ES},   {"ET", BidiClass::ET},
    {"AN", BidiClass::AN},     {"CS", BidiClass::CS},   {"NSM", BidiClass::NSM},
    {"BN", BidiClass::BN},     {"B", BidiClass::B},     {"S", BidiClass::S},
    {"WS", BidiClass::WS},     {"ON", BidiClass::ON},   {"LRE", BidiClass::LRE},
    {"LRO", BidiClass::LRO},   {"RLE", BidiClass::RLE}, {"RLO", BidiClass::RLO},
    {"PDF", BidiClass::PDF},   {"LRI", BidiClass::LRI}, {"RLI", BidiClass::RLI},
    {"FSI", BidiClass::FSI},   {"PDI", BidiClass::PDI},
};

// One representative code point per class. ON is a non-bracket so BidiTest's
// "no paired brackets" assumption holds.
static bool representative(BidiClass c, char32_t *out)
{
    static const std::pair<BidiClass, char32_t> reps[] = {
        {BidiClass::L, 0x0041},   {BidiClass::R, 0x05D0},   {BidiClass::AL, 0x0627},
        {BidiClass::EN, 0x0030},  {BidiClass::ES, 0x002B},  {BidiClass::ET, 0x0024},
        {BidiClass::AN, 0x0660},  {BidiClass::CS, 0x002C},  {BidiClass::NSM, 0x0300},
        {BidiClass::BN, 0x00AD},  {BidiClass::B, 0x2029},   {BidiClass::S, 0x0009},
        {BidiClass::WS, 0x0020},  {BidiClass::ON, 0x0021},  {BidiClass::LRE, 0x202A},
        {BidiClass::LRO, 0x202D}, {BidiClass::RLE, 0x202B}, {BidiClass::RLO, 0x202E},
        {BidiClass::PDF, 0x202C}, {BidiClass::LRI, 0x2066}, {BidiClass::RLI, 0x2067},
        {BidiClass::FSI, 0x2068}, {BidiClass::PDI, 0x2069},
    };
    for (auto const &entry : reps) {
        if (entry.first == c) {
            *out = entry.second;
            return true;
        }
    }
    return false;
}

static std::vector<std::string> split_ws(std::string const &line)
{
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string token;
    while (in >> token) {
        out.push_back(token);
    }
    return out;
}

struct Expected {
    std::vector<int> levels;   // -1 for 'x'
    std::vector<int> reorder;
    bool present = false;
};

static bool parse_levels(std::string const &line, std::vector<int> *out)
{
    out->clear();
    for (std::string const &token : split_ws(line)) {
        if (token == "x") {
            out->push_back(-1);
        } else {
            out->push_back(std::stoi(token));
        }
    }
    return true;
}

static bool parse_reorder(std::string const &line, std::vector<int> *out)
{
    out->clear();
    for (std::string const &token : split_ws(line)) {
        out->push_back(std::stoi(token));
    }
    return true;
}

static void report(char const *file, int line_no, std::string const &detail)
{
    ++g_failures;
    if (g_failures <= 20) {
        std::printf("FAIL %s:%d %s\n", file, line_no, detail.c_str());
    }
}

static std::string levels_to_string(std::vector<uint8_t> const &levels)
{
    std::string out;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i) {
            out += ' ';
        }
        out += std::to_string(levels[i]);
    }
    return out;
}

static std::string order_to_string(std::vector<int> const &order)
{
    std::string out;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (i) {
            out += ' ';
        }
        out += std::to_string(order[i]);
    }
    return out;
}

static void check_one(char const *file, int line_no, std::u32string const &text,
                      std::optional<BidiDirection> base, int expected_paragraph,
                      std::vector<int> const &expected_levels,
                      std::vector<int> const &expected_order)
{
    ++g_checks;
    auto const result = aegir::trinket::analyze_paragraph(text, base);
    if (expected_paragraph >= 0 && result.paragraph_level != expected_paragraph) {
        report(file, line_no,
               "paragraph level " + std::to_string(result.paragraph_level) +
                   " expected " + std::to_string(expected_paragraph));
        return;
    }
    for (std::size_t i = 0; i < expected_levels.size() && i < result.levels.size();
         ++i) {
        if (expected_levels[i] >= 0 &&
            result.levels[i] != static_cast<uint8_t>(expected_levels[i])) {
            report(file, line_no,
                   "levels " + levels_to_string(result.levels) + " expected " +
                       [&] {
                           std::string s;
                           for (std::size_t k = 0; k < expected_levels.size(); ++k) {
                               if (k) {
                                   s += ' ';
                               }
                               s += expected_levels[k] < 0 ? "x"
                                                           : std::to_string(expected_levels[k]);
                           }
                           return s;
                       }());
            return;
        }
    }
    if (result.order != expected_order) {
        report(file, line_no, "order " + order_to_string(result.order) +
                                  " expected " + order_to_string(expected_order));
    }
}

static int run_classes(char const *path)
{
    std::ifstream in(path);
    if (!in) {
        std::printf("cannot open %s\n", path);
        return 2;
    }
    Expected expected;
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (line.rfind("@Levels:", 0) == 0) {
            parse_levels(line.substr(8), &expected.levels);
            expected.present = true;
        } else if (line.rfind("@Reorder:", 0) == 0) {
            parse_reorder(line.substr(9), &expected.reorder);
        } else if (line.empty() || line[0] == '#' || line[0] == '@') {
            continue;
        } else {
            std::size_t const semi = line.find(';');
            if (semi == std::string::npos) {
                continue;
            }
            std::vector<std::string> const names = split_ws(line.substr(0, semi));
            int const bits = std::stoi(line.substr(semi + 1));
            std::u32string text;
            bool ok = true;
            for (std::string const &name : names) {
                BidiClass found = BidiClass::L;
                bool known = false;
                for (auto const &entry : kNames) {
                    if (name == entry.first) {
                        found = entry.second;
                        known = true;
                        break;
                    }
                }
                char32_t rep = 0;
                if (!known || !representative(found, &rep)) {
                    ok = false;
                    break;
                }
                text.push_back(rep);
            }
            if (!ok) {
                report(path, line_no, "unknown class name");
                continue;
            }
            if ((bits & 1) != 0) {
                check_one(path, line_no, text, std::nullopt, -1, expected.levels,
                          expected.reorder);
            }
            if ((bits & 2) != 0) {
                check_one(path, line_no, text, BidiDirection::LTR, -1, expected.levels,
                          expected.reorder);
            }
            if ((bits & 4) != 0) {
                check_one(path, line_no, text, BidiDirection::RTL, -1, expected.levels,
                          expected.reorder);
            }
        }
    }
    return 0;
}

static int run_chars(char const *path)
{
    std::ifstream in(path);
    if (!in) {
        std::printf("cannot open %s\n", path);
        return 2;
    }
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::vector<std::string> fields;
        std::istringstream row(line);
        std::string field;
        while (std::getline(row, field, ';')) {
            fields.push_back(field);
        }
        if (fields.size() < 5) {
            continue;
        }
        std::u32string text;
        for (std::string const &token : split_ws(fields[0])) {
            text.push_back(static_cast<char32_t>(std::stoul(token, nullptr, 16)));
        }
        int const dir = std::stoi(fields[1]);
        int const paragraph = std::stoi(fields[2]);
        std::vector<int> levels;
        parse_levels(fields[3], &levels);
        std::vector<int> order;
        parse_reorder(fields[4], &order);
        std::optional<BidiDirection> base;
        if (dir == 0) {
            base = BidiDirection::LTR;
        } else if (dir == 1) {
            base = BidiDirection::RTL;
        }
        check_one(path, line_no, text, base, paragraph, levels, order);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::printf("usage: %s classes|chars <file>\n", argv[0]);
        return 2;
    }
    int const status = std::strcmp(argv[1], "classes") == 0 ? run_classes(argv[2])
                                                            : run_chars(argv[2]);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return status != 0 ? status : (g_failures == 0 ? 0 : 1);
}
