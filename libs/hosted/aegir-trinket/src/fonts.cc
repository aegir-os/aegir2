/*
 * Trinket font catalog implementation (see fonts.h and specs/fonts.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/fonts.h>

#include "bdf_text.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <utility>

namespace aegir::trinket {

namespace {

/* How much of a face the scan reads: a BDF's properties live in the first
 * lines, and the face itself may be megabytes (specs/fonts.md). */
constexpr size_t kProbeBytes = 4096;

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_dir(std::string const& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

/* A family name is a name: compare case-blind, so a theme's "Terminus" and a
 * header's "terminus" are the same family. */
bool same_family(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char const x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
        char const y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
        if (x != y) return false;
    }
    return true;
}

std::string read_head(std::string const& path, size_t bytes) {
    int const fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return {};
    std::string head(bytes, '\0');
    ssize_t const have = ::read(fd, head.data(), bytes);
    ::close(fd);
    if (have <= 0) return {};
    head.resize(static_cast<size_t>(have));
    return head;
}

}  // namespace

std::string read_file(std::string const& path) {
    int const fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return {};
    std::string bytes;
    /* The size comes from the fd already open, so the buffer is claimed once
     * rather than grown a chunk at a time: a font is hundreds of kilobytes and
     * every reallocation doubles the peak. */
    struct stat info {};
    if (::fstat(fd, &info) == 0 && info.st_size > 0) {
        bytes.reserve(static_cast<size_t>(info.st_size));
    }
    char chunk[512];
    for (;;) {
        ssize_t const have = ::read(fd, chunk, sizeof(chunk));
        if (have < 0) {
            bytes.clear();
            break;
        }
        if (have == 0) break;
        bytes.append(chunk, static_cast<size_t>(have));
    }
    ::close(fd);
    return bytes;
}

bool probe_bdf(std::string_view header, FontFace* out) {
    if (out == nullptr) return false;
    *out = FontFace{};
    bool named = false;
    size_t cursor = 0;
    std::string_view line;
    while (detail::next_line(header, cursor, line)) {
        if (detail::starts_with(line, "FAMILY_NAME")) {
            out->family = detail::property_text(line.substr(11));
            named = !out->family.empty();
        } else if (detail::starts_with(line, "WEIGHT_NAME")) {
            out->weight = detail::property_text(line.substr(11));
        } else if (detail::starts_with(line, "SLANT")) {
            out->slant = detail::property_text(line.substr(5));
        } else if (detail::starts_with(line, "PIXEL_SIZE")) {
            std::string_view rest = line.substr(10);
            detail::next_int(rest, out->pixel_size);
        } else if (detail::starts_with(line, "ENDPROPERTIES") ||
                   detail::starts_with(line, "CHARS")) {
            break;  // the properties are behind us
        }
    }
    return named;
}

FontFace const* select_face(std::vector<FontFace> const& faces, std::string_view family,
                            int pixel_size, bool bold, bool italic) {
    FontFace const* best = nullptr;
    int best_size = 0;
    int best_style = 0;
    for (FontFace const& face : faces) {
        if (!same_family(face.family, family)) continue;
        int const size_delta =
            face.pixel_size > pixel_size ? face.pixel_size - pixel_size : pixel_size - face.pixel_size;
        int const style = (face.bold() == bold ? 0 : 1) + (face.italic() == italic ? 0 : 1);
        if (best == nullptr || size_delta < best_size ||
            (size_delta == best_size && style < best_style)) {
            best = &face;
            best_size = size_delta;
            best_style = style;
        }
    }
    return best;
}

bool FontCatalog::scan(std::string_view root) {
    std::vector<std::string> pending{std::string(root)};
    while (!pending.empty()) {
        std::string const dir = pending.back();
        pending.pop_back();
        DIR* const handle = ::opendir(dir.c_str());
        if (handle == nullptr) continue;
        while (dirent const* const entry = ::readdir(handle)) {
            std::string const name = entry->d_name;
            if (name == "." || name == "..") continue;
            std::string const path = dir + "/" + name;
            if (entry->d_type == DT_DIR || (entry->d_type == DT_UNKNOWN && is_dir(path))) {
                pending.push_back(path);
                continue;
            }
            /* Phase 1 reads BDF faces; an extension the catalog does not know
             * (the OpenType files on the volume before FreeType lands) is
             * skipped, not an error (specs/fonts.md). */
            if (ends_with(name, ".bdf")) {
                FontFace face;
                if (probe_bdf(read_head(path, kProbeBytes), &face)) {
                    face.path = path;
                    faces_.push_back(std::move(face));
                }
            }
        }
        ::closedir(handle);
    }
    return !faces_.empty();
}

}  // namespace aegir::trinket
