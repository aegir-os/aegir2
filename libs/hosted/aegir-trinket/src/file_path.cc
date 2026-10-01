/*
 * Trinket file-requester path and size helpers (see the header).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/file_path.h>

namespace aegir::trinket::file_path {

std::u32string parent_of(std::u32string_view path) {
    /* A trailing separator is not a component: drop it first, so the parent of
     * "AEGIR:Docs/" is "AEGIR:" and not "AEGIR:Docs". */
    while (!path.empty() && path.back() == U'/') {
        path.remove_suffix(1);
    }
    if (path.empty()) {
        return {};
    }
    size_t const slash = path.rfind(U'/');
    if (slash != std::u32string_view::npos) {
        return std::u32string(path.substr(0, slash));
    }
    /* No slash: the path is a volume and its first component, so its parent is
     * the volume root, colon and all. A bare name has no parent but itself. */
    size_t const colon = path.rfind(U':');
    if (colon != std::u32string_view::npos) {
        return std::u32string(path.substr(0, colon + 1));
    }
    return std::u32string(path);
}

std::u32string join(std::u32string_view drawer, std::u32string_view name) {
    if (drawer.empty()) {
        return std::u32string(name);
    }
    if (name.empty()) {
        return std::u32string(drawer);
    }
    std::u32string out(drawer);
    char32_t const last = drawer.back();
    if (last != U':' && last != U'/') {
        out.push_back(U'/');
    }
    out.append(name);
    return out;
}

std::string format_size(uint64_t bytes) {
    std::string const digits = std::to_string(bytes);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) {
            out.push_back(',');
        }
        out.push_back(digits[i]);
    }
    return out;
}

}  // namespace aegir::trinket::file_path
