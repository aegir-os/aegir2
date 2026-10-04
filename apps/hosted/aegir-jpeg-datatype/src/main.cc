/*
 * aegir-jpeg-datatype: the JPEG datatype class (specs/datatypes.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A class service. Whoever opens a file starts it under their own badge, with
 * `datatypes.class` installed by the spawner and the session's namespace among
 * its grants (specs/launch.md's serve kind, specs/libraries.md). It reads the
 * file through that namespace, decodes it once with libs/hosted/aegir-jpeg (the
 * vendored libjpeg-turbo), and serves the frame a page at a time: the caller
 * owns each page it hands over, and the class maps a copy, writes that slice,
 * and unmaps before answering -- so a picture larger than a page is several
 * calls and no pixels cross as words. It is the ilbm and png class's shape, one
 * format over.
 */

#include <aegir/bootstrap.h>
#include <aegir/datatype/decoded.h>
#include <aegir/datatypes.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/ipc/port.h>
#include <aegir/jpeg.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/nmspace.h>
#include <sel4/sel4.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);
/* The allocator's node pool: a service's stack is pages, so this is a static
 * region, not the heap it serves (specs/userland.md). */
alignas(64) unsigned char g_nodes[64 * 1024];

/* The one object this class holds: the decoded image and the path it came
 * from, so a second `info`/`read` of the same file serves the same decode. */
aegir::datatypes::Decoded g_image;
std::string g_path;

/* The slot a caller's page is moved into for one call, reused and deleted when
 * the call is answered (aegir/font.h's transfer page is the same shape). */
seL4_CPtr g_transfer_slot = 0;
constexpr uint32_t kTransferPageBytes = 4096;

bool read_string(uint64_t const *words, uint32_t count, uint32_t &at, std::string &out)
{
    char const *text = nullptr;
    uint32_t length = 0;
    if (!aegir::nmspace::unpack_string(words + at, count - at, aegir::nmspace::kPathMax,
                                       &text, &length)) {
        return false;
    }
    out.assign(text, length);
    at += 1 + (length + 7) / 8;
    return true;
}

bool read_file(std::string const &path, std::vector<uint8_t> &out)
{
    int const fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    out.clear();
    struct stat info {};
    if (::fstat(fd, &info) == 0 && info.st_size > 0) {
        out.reserve(static_cast<std::size_t>(info.st_size));
    }
    char chunk[4096];
    ssize_t have = 0;
    while ((have = ::read(fd, chunk, sizeof(chunk))) > 0) {
        out.insert(out.end(), chunk, chunk + have);
    }
    ::close(fd);
    return !out.empty();
}

bool ensure_decoded(std::string const &path)
{
    if (path == g_path && !g_image.pixels.empty()) {
        return true;
    }
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes)) {
        return false;
    }
    aegir::datatypes::Decoded decoded;
    if (!aegir::datatypes::jpeg::decode(bytes.data(), bytes.size(), decoded)) {
        return false;
    }
    g_image = std::move(decoded);
    g_path = path;
    return true;
}

void drop_transfer() noexcept
{
    if (g_transfer_slot != 0) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_transfer_slot,
                          aegir::bootstrap::cnode_bits());
    }
}

void handle_identify(uint64_t const *words, uint32_t count, uint64_t *reply,
                     uint32_t *reply_count)
{
    reply[0] = 0;
    *reply_count = 1;
    uint32_t at = 0;
    std::string path;
    if (!read_string(words, count, at, path)) {
        return;
    }
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes)) {
        return;
    }
    reply[0] = aegir::datatypes::jpeg::identify(bytes.data(), bytes.size()) ? 1 : 0;
}

void handle_info(uint64_t const *words, uint32_t count, uint64_t *reply,
                 uint32_t *reply_count)
{
    reply[0] = 0;
    *reply_count = 1;
    uint32_t at = 0;
    std::string path;
    if (!read_string(words, count, at, path) || !ensure_decoded(path)) {
        return;
    }
    aegir::datatypes::Info const &info = g_image.info;
    reply[0] = 1;
    reply[1] = info.width;
    reply[2] = info.height;
    reply[3] = static_cast<uint64_t>(info.format);
    reply[4] = info.stride;
    reply[5] = info.palette_size;
    reply[6] = (info.transparent ? 1u : 0u) |
               (static_cast<uint64_t>(info.transparent_index) << 16);
    *reply_count = 1 + aegir::datatypes::kInfoWords;
}

void handle_read(uint64_t const *words, uint32_t count, bool cap_arrived, uint64_t *reply,
                 uint32_t *reply_count)
{
    reply[0] = 0;
    *reply_count = 1;
    /* The page is picked up whatever happens next: a capability left in the
     * scratch receive slot is the next transfer's problem (ipc/port.h). */
    drop_transfer();
    if (!cap_arrived) {
        return;
    }
    if (!aegir::ipc::take_received_cap(g_transfer_slot)) {
        return;
    }
    uint32_t at = 0;
    std::string path;
    if (!read_string(words, count, at, path) || at >= count) {
        drop_transfer();
        return;
    }
    uint64_t const offset = words[at++];
    if (!ensure_decoded(path)) {
        drop_transfer();
        return;
    }
    uint8_t *const page = static_cast<uint8_t *>(g_scratch.map(g_transfer_slot));
    if (page == nullptr) {
        drop_transfer();
        return;
    }
    std::size_t const written = g_image.read(offset, page, kTransferPageBytes);
    g_scratch.unmap(g_transfer_slot);
    drop_transfer();
    reply[0] = 1;
    reply[1] = written;
    *reply_count = 2;
}

void handle_dispose(uint64_t *reply, uint32_t *reply_count)
{
    g_image = aegir::datatypes::Decoded{};
    g_path.clear();
    reply[0] = 1;
    *reply_count = 1;
}

/* The untyped, the VSpace root and the window the spawner gave this class, and
 * the heap over them (specs/cxx.md; the cxx-smoke's stand-up is the same). */
bool adopt_memory()
{
    g_objects.adopt_nodes(g_nodes, sizeof(g_nodes));
    uint64_t untyped_slot = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    if (!aegir::bootstrap::capability("untyped", 7, &untyped_slot) ||
        !aegir::bootstrap::capability("vspace", 6, &vspace_slot) ||
        !aegir::bootstrap::window(&window_base, &window_bytes)) {
        return false;
    }
    uint64_t untyped_physical = 0;
    uint32_t untyped_bits = 0;
    uint64_t untyped_address = 0;
    static_cast<void>(
        aegir::bootstrap::untyped(&untyped_physical, &untyped_bits, &untyped_address));
    if (!g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                 untyped_physical)) {
        return false;
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
    g_objects.adopt_slots(first_free, total_slots - first_free, 0,
                          aegir::bootstrap::cnode_bits());
    if (!g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                         static_cast<uintptr_t>(window_base),
                         static_cast<uintptr_t>(window_base + window_bytes), &g_objects)) {
        return false;
    }
    constexpr uint64_t kHeapBytes = 16ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        return false;
    }
    g_transfer_slot = g_objects.alloc_slot();
    return g_transfer_slot != 0;
}

} // namespace

int main()
{
    if (!adopt_memory()) {
        aegir::debug_write("  jpeg: FAIL no memory to stand up\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The endpoint the opener made and the spawner installed: the class serves
     * it. */
    aegir::ipc::Owner port = aegir::ipc::Owner::find(
        aegir::datatypes::kClassPortName, aegir::datatypes::kClassPortNameLength);
    if (!port.valid()) {
        aegir::debug_write("  jpeg: FAIL no datatypes.class port to own\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

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
        if (method == aegir::datatypes::kMethodIdentify) {
            handle_identify(words, count, reply, &reply_count);
        } else if (method == aegir::datatypes::kMethodInfo) {
            handle_info(words, count, reply, &reply_count);
        } else if (method == aegir::datatypes::kMethodRead) {
            handle_read(words, count, cap_arrived, reply, &reply_count);
        } else if (method == aegir::datatypes::kMethodDispose) {
            handle_dispose(reply, &reply_count);
        } else {
            reply[0] = 0;
            reply_count = 1;
        }
        port.reply_words(reply, reply_count);
    }
}
