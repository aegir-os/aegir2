/*
 * aegir-font: the font service (specs/fonts.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One service owns the faces `Sys:Fonts` holds and one FreeType; a client owns
 * its own atlas and asks for what it needs. Faces are found by their own
 * metadata and not by where they sit: the tree is scanned recursively and each
 * file is opened through FreeType to read its family and style, so moving a
 * file between folders changes nothing (specs/fonts.md).
 *
 * FreeType never touches the file layer here. Its stream is ours -- a small
 * descriptor that reads through the VFS -- because a face can be nineteen
 * megabytes (the CJK collection) and one VFS read carries 936 bytes: a stream
 * reads the tables a face needs, where reading a whole face into memory would
 * be tens of thousands of round trips (specs/fonts.md).
 *
 * A face is opened on demand and the service holds it until its client closes
 * it. Glyphs are rasterized on demand into a page the *caller* owns and hands
 * over for one call: the service maps a copy of the capability, writes, and
 * unmaps before answering, so the page is the caller's again between calls
 * (specs/fonts.md).
 */

/* musl's <string.h> first, before any seL4 header: seL4's RISC-V syscall
 * header declares strcpy at file scope (seL4_DebugNameThread uses it), and a
 * later `extern "C"` declaration of strcpy -- which is what the C library's
 * header has -- conflicts with that C++ one. With the C header first, strcpy
 * has C linkage already and seL4's plain redeclaration inherits it. FreeType's
 * own headers pull <string.h> in, so the order is fixed here. */
#include <string.h>
#include <time.h>

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/font.h>
#include <aegir/heap.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/memory.h>
#include <aegir/nmspace.h>
#include <sel4/sel4.h>

#include "probe.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_SYSTEM_H

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/* The root the faces live under. It is a name, not a path: the tree's shape is
 * for a person, and the scan is what finds a face (specs/fonts.md). */
constexpr char const kFontsRoot[] = "Sys:Fonts";

void write(char const *text) noexcept
{
    aegir::debug_write("  font: ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

/* Static, like every service's: an allocator carries the tables of what it
 * handed out, and a service's stack is pages. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);
alignas(64) unsigned char g_nodes[64 * 1024];

FT_Library g_library = nullptr;

/* FreeType's allocator, ours so a failure can say what it could not get: a
 * face the rasterizer would not fit is otherwise an error code. What is
 * counted is the requests that came before the failure; FreeType's free
 * callback is not told the size it is freeing, so the bytes held are not
 * tracked (specs/fonts.md). */
struct FtMemory {
    uint64_t requests = 0;
};

FtMemory g_ft_memory;

void *ft_alloc(FT_Memory, long size) noexcept
{
    void *block = std::malloc(static_cast<size_t>(size));
    if (block == nullptr) {
        std::string line("FAIL FreeType could not allocate ");
        line += std::to_string(size);
        line += " bytes after ";
        line += std::to_string(g_ft_memory.requests);
        line += " allocations";
        write(line.c_str());
        return nullptr;
    }
    ++g_ft_memory.requests;
    return block;
}

void *ft_realloc(FT_Memory, long current, long size, void *block) noexcept
{
    void *grown = std::realloc(block, static_cast<size_t>(size));
    if (grown == nullptr) {
        std::string line("FAIL FreeType could not grow ");
        line += std::to_string(current);
        line += " to ";
        line += std::to_string(size);
        line += " bytes";
        write(line.c_str());
        return nullptr;
    }
    ++g_ft_memory.requests;
    return grown;
}

void ft_free(FT_Memory, void *block) noexcept
{
    std::free(block);
}

FT_MemoryRec_ g_ft_memory_rec{};

/* ---- the stream FreeType reads through (specs/fonts.md) ---- */

/* One open file, as FreeType's stream sees it. The stream is not FreeType's
 * file layer: its read is ours and goes to the VFS through the fd. */
struct Source {
    FT_StreamRec stream{};
    int fd = -1;
};

unsigned long stream_read(FT_Stream stream, unsigned long offset, unsigned char *buffer,
                          unsigned long count) noexcept
{
    auto *source = static_cast<Source *>(stream->descriptor.pointer);
    if (source == nullptr || source->fd < 0) {
        return 0;
    }
    if (::lseek(source->fd, static_cast<long>(offset), SEEK_SET) < 0) {
        return 0;
    }
    if (buffer == nullptr || count == 0) {
        return 0; /* a seek: FreeType only wants the position moved */
    }
    ssize_t const have = ::read(source->fd, buffer, count);
    return have <= 0 ? 0 : static_cast<unsigned long>(have);
}

void stream_close(FT_Stream stream) noexcept
{
    auto *source = static_cast<Source *>(stream->descriptor.pointer);
    if (source != nullptr && source->fd >= 0) {
        ::close(source->fd);
        source->fd = -1;
    }
}

/* A face and the stream that feeds it. The stream's memory is ours, so it is
 * released here rather than in FreeType's close (which only closes the fd). */
struct OpenFace {
    FT_Face face = nullptr;
    Source *source = nullptr;

    OpenFace() = default;
    OpenFace(OpenFace const &) = delete;
    OpenFace &operator=(OpenFace const &) = delete;
    OpenFace(OpenFace &&other) noexcept : face(other.face), source(other.source)
    {
        other.face = nullptr;
        other.source = nullptr;
    }
    OpenFace &operator=(OpenFace &&other) noexcept
    {
        reset();
        face = other.face;
        source = other.source;
        other.face = nullptr;
        other.source = nullptr;
        return *this;
    }
    ~OpenFace() { reset(); }

    void reset() noexcept
    {
        if (face != nullptr) {
            FT_Done_Face(face);
            face = nullptr;
        }
        delete source;
        source = nullptr;
    }
    bool valid() const { return face != nullptr; }
};

/* Open one face of one file through a VFS-backed stream. A failed open is not
 * an error worth a line: the tree is walked by name and a file that is not a
 * face is simply not one (specs/fonts.md). `why`, when given, names the caller
 * so a failure the service did not expect can say so. */
bool open_face(std::string const &path, long index, OpenFace *out,
               char const *why = nullptr)
{
    int const fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (why != nullptr) {
            std::string line("FAIL ");
            line += why;
            line += ": ";
            line += path;
            line += " would not open";
            write(line.c_str());
        }
        return false;
    }
    long const size = ::lseek(fd, 0, SEEK_END);
    if (size <= 0) {
        if (why != nullptr) {
            std::string line("FAIL ");
            line += why;
            line += ": ";
            line += path;
            line += " has no size";
            write(line.c_str());
        }
        ::close(fd);
        return false;
    }
    ::lseek(fd, 0, SEEK_SET);

    auto *source = new (std::nothrow) Source();
    if (source == nullptr) {
        ::close(fd);
        return false;
    }
    source->fd = fd;
    source->stream.size = static_cast<unsigned long>(size);
    source->stream.descriptor.pointer = source;
    source->stream.read = stream_read;
    source->stream.close = stream_close;

    FT_Open_Args args{};
    args.flags = FT_OPEN_STREAM;
    args.stream = reinterpret_cast<FT_Stream>(&source->stream);
    FT_Face face = nullptr;
    FT_Error const error = FT_Open_Face(g_library, &args, index, &face);
    if (error != 0 || face == nullptr) {
        if (why != nullptr) {
            std::string line("FAIL ");
            line += why;
            line += ": ";
            line += path;
            line += " index ";
            line += std::to_string(index);
            line += " of ";
            line += std::to_string(size);
            line += " bytes, FreeType error ";
            line += std::to_string(static_cast<int>(error));
            write(line.c_str());
        }
        if (source->fd >= 0) {
            ::close(source->fd);
        }
        delete source;
        return false;
    }
    out->face = face;
    out->source = source;
    return true;
}

/* ---- the index (specs/fonts.md) ---- */

/* One face the scan found: the face's own name and where it is. The path is
 * not the index; it is only how the face is reached again. */
struct IndexedFace {
    std::string family;
    std::string style;
    std::string path;
    long index = 0;
    bool scalable = false;
    int pixel_size = 0; /* a bitmap-only face's own size */
};

bool is_dir(std::string const &path)
{
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

/* Open every face of one file and record its name. The file may hold several
 * faces (a TrueType collection): `num_faces` is FreeType's count, and each is
 * opened by its own index. */
/* The reader the probe asks for: the file's bytes at an offset. `context` is
 * the open fd. */
bool read_at(void *context, uint64_t offset, uint64_t length, void *out) noexcept
{
    int const fd = *static_cast<int *>(context);
    if (::lseek(fd, static_cast<long>(offset), SEEK_SET) < 0) {
        return false;
    }
    auto *bytes = static_cast<char *>(out);
    uint64_t total = 0;
    while (total < length) {
        ssize_t const have = ::read(fd, bytes + total, length - total);
        if (have <= 0) {
            return false;
        }
        total += static_cast<uint64_t>(have);
    }
    return true;
}

/* Read a face's own name from the file, without opening it through FreeType:
 * the name is a few kilobytes and a CJK face is megabytes, so opening sixteen
 * of them to index the volume cost twenty seconds of a boot (probe.h,
 * specs/fonts.md). FreeType is left the rendering. */
void probe_file(std::string const &path, std::vector<IndexedFace> &faces)
{
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return;
    }
    long const size = ::lseek(fd, 0, SEEK_END);
    if (size <= 0) {
        ::close(fd);
        return;
    }
    aegir::font::Reader reader;
    reader.context = &fd;
    reader.size = static_cast<uint64_t>(size);
    reader.at = read_at;
    for (aegir::font::FaceInfo &info : aegir::font::probe_file(reader)) {
        IndexedFace record;
        record.family = std::move(info.family);
        record.style = std::move(info.style);
        record.scalable = info.scalable;
        record.pixel_size = info.pixel_size;
        record.index = static_cast<long>(info.index);
        record.path = path;
        std::string line("face ");
        line += std::to_string(record.index);
        line += ": ";
        line += record.family;
        line += " (";
        line += record.style;
        line += ")";
        write(line.c_str());
        faces.push_back(std::move(record));
    }
    ::close(fd);
}

/* Scan a root recursively. A directory that will not open is skipped, not
 * fatal: a font service that cannot read one folder should still serve the
 * rest (specs/fonts.md). */
void scan(std::string const &root, std::vector<IndexedFace> &faces)
{
    std::vector<std::string> pending{root};
    while (!pending.empty()) {
        std::string const dir = pending.back();
        pending.pop_back();
        DIR *const handle = ::opendir(dir.c_str());
        if (handle == nullptr) {
            continue;
        }
        while (dirent const *const entry = ::readdir(handle)) {
            std::string const name = entry->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            std::string const path = dir + "/" + name;
            if (entry->d_type == DT_DIR || (entry->d_type == DT_UNKNOWN && is_dir(path))) {
                pending.push_back(path);
                continue;
            }
            probe_file(path, faces);
        }
        ::closedir(handle);
    }
}

/* The face a request picks: the family's, then the size nearest the ask, then
 * the style nearest it. A scalable face answers any size, so its size is only a
 * tie-break; a bitmap face's own size is what it has. */
IndexedFace const *select_face(std::vector<IndexedFace> const &faces, std::string_view family,
                               int size, bool bold, bool italic)
{
    auto lower = [](std::string_view text) {
        std::string out(text);
        for (char &c : out) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + 32);
            }
        }
        return out;
    };
    auto is_bold = [](std::string const &style) {
        return style.find("Bold") != std::string::npos ||
               style.find("Black") != std::string::npos ||
               style.find("Demi") != std::string::npos;
    };
    auto is_italic = [](std::string const &style) {
        return style.find("Italic") != std::string::npos ||
               style.find("Oblique") != std::string::npos;
    };

    std::string const wanted = lower(family);
    IndexedFace const *best = nullptr;
    int best_size = 0;
    int best_style = 0;
    for (IndexedFace const &face : faces) {
        if (lower(face.family) != wanted) {
            continue;
        }
        int const delta = face.scalable || face.pixel_size == 0
                              ? 0
                              : (face.pixel_size > size ? face.pixel_size - size
                                                        : size - face.pixel_size);
        int const style = (is_bold(face.style) == bold ? 0 : 1) +
                          (is_italic(face.style) == italic ? 0 : 1);
        if (best == nullptr || delta < best_size ||
            (delta == best_size && style < best_style)) {
            best = &face;
            best_size = delta;
            best_style = style;
        }
    }
    return best;
}

/* ---- the open faces a client holds ---- */

struct OpenServerFace {
    OpenFace open;
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    int height = 0;
};

std::vector<std::unique_ptr<OpenServerFace>> g_open_faces;
std::vector<IndexedFace> g_index;

uint64_t store_face(OpenServerFace *face)
{
    for (size_t i = 0; i < g_open_faces.size(); ++i) {
        if (g_open_faces[i] == nullptr) {
            g_open_faces[i] = std::unique_ptr<OpenServerFace>(face);
            return static_cast<uint64_t>(i) + 1;
        }
    }
    g_open_faces.emplace_back(face);
    return static_cast<uint64_t>(g_open_faces.size());
}

OpenServerFace *lookup_face(uint64_t id)
{
    if (id == 0 || id > g_open_faces.size()) {
        return nullptr;
    }
    return g_open_faces[static_cast<size_t>(id - 1)].get();
}

/* Load the selected face and take its metrics. */
OpenServerFace *load_face(IndexedFace const &record, int size)
{
    auto loaded = std::make_unique<OpenServerFace>();
    if (!open_face(record.path, record.index, &loaded->open, "load")) {
        return nullptr;
    }
    FT_Face face = loaded->open.face;
    if ((face->face_flags & FT_FACE_FLAG_SCALABLE) != 0) {
        FT_Error const error = FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(size));
        if (error != 0) {
            std::string line("FAIL load: FT_Set_Pixel_Sizes(");
            line += std::to_string(size);
            line += ") returned ";
            line += std::to_string(static_cast<int>(error));
            write(line.c_str());
            return nullptr;
        }
    } else if (face->num_fixed_sizes > 1) {
        /* A face with several strikes: take the one nearest the ask. FreeType
         * picks a default strike otherwise, which may not be it. */
        int best = 0;
        int best_delta = -1;
        for (int i = 0; i < face->num_fixed_sizes; ++i) {
            int const strike = static_cast<int>(face->available_sizes[i].y_ppem >> 6);
            int const delta = strike > size ? strike - size : size - strike;
            if (best_delta < 0 || delta < best_delta) {
                best = i;
                best_delta = delta;
            }
        }
        static_cast<void>(FT_Select_Size(face, best));
    }
    FT_Size_Metrics const &metrics = face->size->metrics;
    loaded->ascent = static_cast<int>(metrics.ascender >> 6);
    loaded->descent = static_cast<int>(metrics.descender >> 6);
    loaded->height = static_cast<int>(metrics.height >> 6);
    loaded->line_gap = loaded->height - (loaded->ascent + loaded->descent);
    return loaded.release();
}

/* ---- the port ---- */

uint32_t read_string(uint64_t const *words, uint32_t count, uint32_t *at, std::string *out)
{
    char const *text = nullptr;
    uint32_t length = 0;
    if (!aegir::nmspace::unpack_string(words + *at, count - *at, aegir::nmspace::kPathMax, &text,
                                       &length)) {
        return 0;
    }
    out->assign(text, length);
    *at += 1 + (length + 7) / 8;
    return 1;
}

/* open: family, size, bold, italic. Answer: found, then the id. */
void handle_open(uint64_t const *words, uint32_t count, uint64_t *reply, uint32_t *reply_count)
{
    reply[0] = 0;
    *reply_count = 1;
    if (count < 1) {
        return;
    }
    uint32_t at = 0;
    std::string family;
    if (read_string(words, count, &at, &family) == 0 || at + 3 > count) {
        return;
    }
    int const size = static_cast<int>(words[at++]);
    bool const bold = words[at++] != 0;
    bool const italic = words[at] != 0;

    IndexedFace const *const record = select_face(g_index, family, size, bold, italic);
    if (record == nullptr) {
        return;
    }
    OpenServerFace *const loaded = load_face(*record, size);
    if (loaded == nullptr) {
        return;
    }
    reply[0] = 1;
    reply[1] = store_face(loaded);
    *reply_count = 2;
}

/* metrics: the id. Answer: found, then ascent, descent, line gap, height. */
void handle_metrics(uint64_t const *words, uint32_t count, uint64_t *reply,
                    uint32_t *reply_count)
{
    reply[0] = 0;
    *reply_count = 1;
    if (count < 1) {
        return;
    }
    OpenServerFace *const face = lookup_face(words[0]);
    if (face == nullptr) {
        return;
    }
    reply[0] = 1;
    reply[1] = static_cast<uint64_t>(face->ascent);
    reply[2] = static_cast<uint64_t>(face->descent);
    reply[3] = static_cast<uint64_t>(face->line_gap);
    reply[4] = static_cast<uint64_t>(face->height);
    *reply_count = 5;
}

/* close: the id. Answer: closed. */
void handle_close(uint64_t const *words, uint32_t count, uint64_t *reply, uint32_t *reply_count)
{
    reply[0] = 0;
    *reply_count = 1;
    if (count < 1) {
        return;
    }
    OpenServerFace *const face = lookup_face(words[0]);
    if (face == nullptr) {
        return;
    }
    g_open_faces[static_cast<size_t>(words[0] - 1)].reset();
    reply[0] = 1;
}

/* ---- the caller's transfer page (specs/fonts.md) ---- */

/* The slot a client's page capability is moved into for one call, reused: the
 * capability is deleted when the call is answered, so the next install finds
 * the slot empty (take_received_cap moves with CNode_Move, which refuses an
 * occupied destination), and a long-lived service does not leak a slot per
 * glyph batch. */
seL4_CPtr g_transfer_slot = 0;

void drop_transfer() noexcept
{
    if (g_transfer_slot != 0) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_transfer_slot,
                          aegir::bootstrap::cnode_bits());
    }
}

/* One rasterized glyph's coverage into the page, as 8-bit alpha. FreeType
 * hands the two shapes a face here uses: 8-bit gray for an outline face, and
 * 1-bit packed for a bitmap face (a BDF). True when it was written -- a blank
 * glyph (a space) has nothing to copy and is written as nothing. */
bool write_coverage(FT_Bitmap const &bitmap, uint8_t *page, size_t at) noexcept
{
    size_t const width = bitmap.width;
    size_t const rows = bitmap.rows;
    if (width == 0 || rows == 0) {
        return true;
    }
    int const pitch = static_cast<int>(bitmap.pitch);
    if (bitmap.buffer == nullptr || pitch <= 0) {
        return false;
    }
    uint8_t *const out = page + at;
    if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        for (size_t y = 0; y < rows; ++y) {
            memcpy(out + y * width, bitmap.buffer + y * static_cast<size_t>(pitch), width);
        }
        return true;
    }
    if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
        for (size_t y = 0; y < rows; ++y) {
            uint8_t const *const row = bitmap.buffer + y * static_cast<size_t>(pitch);
            for (size_t x = 0; x < width; ++x) {
                bool const on = (row[x / 8] & (0x80u >> (x % 8))) != 0;
                out[y * width + x] = on ? 255 : 0;
            }
        }
        return true;
    }
    return false;
}

/* glyphs: the id, the count, the codepoints and the caller's page. Answer: 1,
 * how many were answered, and a record each (aegir/font.h). The page bounds the
 * pixels and the envelope bounds the records, so the answer is a prefix of the
 * request: a caller whose batch was trimmed asks again for the rest. */
void handle_glyphs(uint64_t const *words, uint32_t count, bool cap_arrived,
                   uint64_t *reply, uint32_t *reply_count)
{
    reply[0] = 0;
    *reply_count = 1;
    /* The page is picked up whatever happens next: a capability left in the
     * scratch receive slot is the *next* transfer's problem (ipc/port.h). */
    drop_transfer();
    if (!cap_arrived || count < 2) {
        return;
    }
    if (!aegir::ipc::take_received_cap(g_transfer_slot)) {
        return;
    }
    uint32_t const asked = static_cast<uint32_t>(words[1]);
    OpenServerFace *const face = lookup_face(words[0]);
    if (face == nullptr || face->open.face == nullptr || asked > count - 2) {
        drop_transfer();
        return;
    }
    uint8_t *const page = static_cast<uint8_t *>(g_scratch.map(g_transfer_slot));
    if (page == nullptr) {
        drop_transfer();
        return;
    }

    uint32_t const record_room =
        (aegir::ipc::kMaxWords - 2) / aegir::font::kGlyphsWordsPerCode;
    uint32_t const limit = asked < record_room ? asked : record_room;
    FT_Face const ft = face->open.face;
    size_t used = 0;
    uint32_t answered = 0;
    uint32_t at = 2;
    for (uint32_t i = 0; i < limit; ++i) {
        auto const code = static_cast<FT_ULong>(words[2 + i]);
        int advance = 0;
        int bearing_x = 0;
        int bearing_y = 0;
        int width = 0;
        int height = 0;
        size_t offset = 0;
        bool present = false;

        FT_UInt const index = FT_Get_Char_Index(ft, code);
        if (index != 0 && FT_Load_Glyph(ft, index, FT_LOAD_RENDER) == 0) {
            FT_GlyphSlot const slot = ft->glyph;
            FT_Bitmap const &bitmap = slot->bitmap;
            size_t const bytes = static_cast<size_t>(bitmap.width) * bitmap.rows;
            present = true;
            advance = static_cast<int>(slot->advance.x >> 6);
            bearing_x = slot->bitmap_left;
            bearing_y = slot->bitmap_top;
            if (bytes == 0) {
                /* A blank glyph: a space carries an advance and no ink. */
            } else if (bytes > aegir::font::kTransferPageBytes) {
                /* Bigger than a page: no call could carry it, so it is
                 * answered as the advance alone rather than asked forever. */
                width = 0;
                height = 0;
            } else if (bytes > aegir::font::kTransferPageBytes - used) {
                /* The page is full: the caller asks again from the top, and
                 * the next call's page is empty. */
                break;
            } else if (write_coverage(bitmap, page, used)) {
                width = static_cast<int>(bitmap.width);
                height = static_cast<int>(bitmap.rows);
                offset = used;
                used += bytes;
            } else {
                present = false;
            }
        }
        reply[at++] = static_cast<uint64_t>(advance);
        reply[at++] = static_cast<uint64_t>(bearing_x);
        reply[at++] = static_cast<uint64_t>(bearing_y);
        reply[at++] = static_cast<uint64_t>(width);
        reply[at++] = static_cast<uint64_t>(height);
        reply[at++] = static_cast<uint64_t>(offset);
        reply[at++] = present ? 1 : 0;
        ++answered;
    }

    g_scratch.unmap(g_transfer_slot);
    drop_transfer();
    reply[0] = 1;
    reply[1] = answered;
    *reply_count = at;
}

/* The service's own proof that the disk's faces load and measure, and what the
 * acceptance cues on: a face is opened by name, its metrics read, and they are
 * checked for the shape a face's metrics must have (specs/fonts.md). */
void self_check()
{
    constexpr char const kFamily[] = "Noto Sans";
    IndexedFace const *const record = select_face(g_index, kFamily, 16, false, false);
    if (record == nullptr) {
        write("no face for Noto Sans");
        return;
    }
    std::unique_ptr<OpenServerFace> face(load_face(*record, 16));
    if (face == nullptr) {
        write("FAIL Noto Sans 16 would not load");
        return;
    }
    /* The metrics a face must have: a positive ascent, a non-positive descent
     * and a line height at least the ascent. `height` is FreeType's
     * recommended line spacing and is not ascent minus descent -- Noto Sans at
     * 16 is 18, -5, 22 -- so the two are checked apart. */
    bool const sane = face->ascent > 0 && face->descent <= 0 && face->height >= face->ascent;
    std::string line;
    if (sane) {
        line = "open Noto Sans 16 is sane: ascent ";
    } else {
        line = "FAIL open Noto Sans 16: ascent ";
    }
    line += std::to_string(face->ascent);
    line += ", descent ";
    line += std::to_string(face->descent);
    line += ", height ";
    line += std::to_string(face->height);
    write(line.c_str());

    /* And it draws: a glyph is rasterized here, so the cue is a box and not
     * just a measurement (specs/fonts.md). The pixels cross a client's page in
     * a real call; this is the rasterizer's own proof. */
    if (FT_Load_Char(face->open.face, 'A', FT_LOAD_RENDER) == 0) {
        FT_GlyphSlot const slot = face->open.face->glyph;
        std::string glyph("glyph A ");
        glyph += std::to_string(slot->bitmap.width);
        glyph += "x";
        glyph += std::to_string(slot->bitmap.rows);
        glyph += " at ";
        glyph += std::to_string(slot->bitmap_left);
        glyph += ",";
        glyph += std::to_string(slot->bitmap_top);
        glyph += ", advance ";
        glyph += std::to_string(static_cast<int>(slot->advance.x >> 6));
        write(glyph.c_str());
    } else {
        write("FAIL Noto Sans 16 would not rasterize A");
    }
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        static_cast<void>(log.call(aegir::log::kMethodEvent,
                                   static_cast<uint64_t>(aegir::log::Event::Starting)));
    }

    /* The endpoint director made: the owner half of font.main. */
    aegir::ipc::Owner port =
        aegir::ipc::Owner::find(aegir::font::kPortName, aegir::font::kPortNameLength);
    if (!port.valid()) {
        write("FAIL no font.main port to own");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    g_objects.adopt_nodes(g_nodes, sizeof(g_nodes));

    uint64_t untyped_slot = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    if (!aegir::bootstrap::capability("untyped", 7, &untyped_slot) ||
        !aegir::bootstrap::capability("vspace", 6, &vspace_slot) ||
        !aegir::bootstrap::window(&window_base, &window_bytes)) {
        write("FAIL no memory and no address space were given");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    uint64_t untyped_physical = 0;
    uint32_t untyped_bits = 0;
    uint64_t untyped_address = 0;
    static_cast<void>(
        aegir::bootstrap::untyped(&untyped_physical, &untyped_bits, &untyped_address));
    if (!g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                 untyped_physical)) {
        write("FAIL the memory I was given would not adopt");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            if (entry.kind == aegir::bootstrap::EntryKind::Capability &&
                entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            }
        }
    }
    uint64_t const total_slots = 1ull << aegir::bootstrap::cnode_bits();
    g_objects.adopt_slots(first_free, total_slots - first_free, 0);
    if (!g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                         static_cast<uintptr_t>(window_base),
                         static_cast<uintptr_t>(window_base + window_bytes), &g_objects)) {
        write("FAIL the window I was given could not be adopted");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    /* The heap is a font service's, not the toolkit's: a face in the CJK
     * collection carries a sixteen-megabyte CFF table, and a stream still
     * holds a face's rasterizer state. The region is address space -- physical
     * pages are retyped as they are touched -- so the size costs nothing until
     * a face needs it (specs/fonts.md). */
    constexpr uint64_t kHeapBytes = 64ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        write("FAIL the heap would not claim the window");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    /* The one slot a client's transfer page is moved into (specs/fonts.md). */
    g_transfer_slot = g_objects.alloc_slot();
    if (g_transfer_slot == 0) {
        write("FAIL no CSpace slot for the transfer page");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* What the service was born with, said once, because a face that will not
     * fit is otherwise a mystery: the untyped it retypes from, the window it
     * maps, and the CSpace slots a retype consumes. */
    {
        std::string bootinfo_line("untyped ");
        bootinfo_line += std::to_string(untyped_bits);
        bootinfo_line += " bits, window ";
        bootinfo_line += std::to_string(window_bytes);
        bootinfo_line += " bytes, cspace ";
        bootinfo_line += std::to_string(aegir::bootstrap::cnode_bits());
        bootinfo_line += " bits (first free slot ";
        bootinfo_line += std::to_string(first_free);
        bootinfo_line += "), mem.main ";
        aegir::ipc::Consumer const memory_client =
            aegir::ipc::Consumer::find(aegir::memory::kPortName, aegir::memory::kPortNameLength);
        bootinfo_line += memory_client.valid() ? "mine" : "missing";
        write(bootinfo_line.c_str());
    }

    g_ft_memory_rec.user = nullptr;
    g_ft_memory_rec.alloc = ft_alloc;
    g_ft_memory_rec.realloc = ft_realloc;
    g_ft_memory_rec.free = ft_free;
    if (FT_New_Library(&g_ft_memory_rec, &g_library) != 0) {
        write("FAIL FreeType would not initialize");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    FT_Add_Default_Modules(g_library);

    /* The system volume comes after this service starts: the namespace exists
     * first, the disk answers later. Wait for the root, without a line per
     * attempt -- a service that cannot say what it is doing is a service nobody
     * can supervise, and one that says it a million times is one nobody can
     * read. */
    for (;;) {
        DIR *const root = ::opendir(kFontsRoot);
        if (root != nullptr) {
            ::closedir(root);
            break;
        }
        seL4_Yield();
    }

    timespec before {};
    timespec after {};
    static_cast<void>(clock_gettime(CLOCK_MONOTONIC, &before));
    scan(kFontsRoot, g_index);
    static_cast<void>(clock_gettime(CLOCK_MONOTONIC, &after));

    std::string found = std::to_string(g_index.size());
    found += " faces from Sys:Fonts in ";
    long const elapsed_ms = (after.tv_sec - before.tv_sec) * 1000 +
                            (after.tv_nsec - before.tv_nsec) / 1000000;
    found += std::to_string(elapsed_ms);
    found += " ms";
    write(found.c_str());
    if (g_index.empty()) {
        write("FAIL Sys:Fonts holds no face this service can read");
    }
    self_check();

    if (log.valid()) {
        static_cast<void>(log.call(aegir::log::kMethodEvent,
                                   static_cast<uint64_t>(aegir::log::Event::Ready)));
    }
    write("ready, serving font.main");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    for (;;) {
        uint64_t words[aegir::ipc::kMaxWords];
        uint32_t count = 0;
        seL4_Word badge = 0;
        bool cap_arrived = false;
        uint32_t const method = port.receive_words(words, aegir::ipc::kMaxWords, &count,
                                                   &badge, &cap_arrived);
        static_cast<void>(badge);
        uint64_t reply[aegir::ipc::kMaxWords] = {};
        uint32_t reply_count = 0;
        if (method == aegir::font::kMethodOpen) {
            handle_open(words, count, reply, &reply_count);
        } else if (method == aegir::font::kMethodMetrics) {
            handle_metrics(words, count, reply, &reply_count);
        } else if (method == aegir::font::kMethodClose) {
            handle_close(words, count, reply, &reply_count);
        } else if (method == aegir::font::kMethodGlyphs) {
            handle_glyphs(words, count, cap_arrived, reply, &reply_count);
        } else {
            reply[0] = 0;
            reply_count = 1;
        }
        port.reply_words(reply, reply_count);
    }
}
