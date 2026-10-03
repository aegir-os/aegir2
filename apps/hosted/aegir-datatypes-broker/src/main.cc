/*
 * aegir-datatypes-broker: the session's datatypes broker (specs/datatypes.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A session service declared in Sys:S/session.manifest: auth starts it with the
 * session's own authority and it owns `datatypes.main`. When a caller opens a
 * file it makes the class port, hands the broker the other half, and the broker
 * serve-launches a class -- under the caller's user class, through the session
 * launcher -- and asks the class to identify the file. A class, not a table, is
 * the authority on what it reads (specs/datatypes.md).
 */

/* The C++ standard headers first: libsel4's riscv syscalls.h declares strcpy
 * with C++ linkage, so it must follow the C library's C-linkage declaration
 * (the terminal's include order is the same, for the same reason). */
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <aegir/bootstrap.h>
#include <aegir/datatypes.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/ipc/port.h>
#include <aegir/launch_client.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/nmspace.h>
#include <sel4/sel4.h>

namespace {

aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);
/* A service's stack is pages, so the allocator's node pool is a static region,
 * not the heap it serves (the class service's stand-up is the same). */
alignas(64) unsigned char g_nodes[64 * 1024];

/* The slot a caller's class port is moved into for one open, reused and
 * deleted when the open is answered. */
seL4_CPtr g_class_slot = 0;

/* The class a file's extension hints at: which class to ask *first*, not which
 * one answers (specs/datatypes.md). The class, not the name, decides. */
std::string class_from_extension(std::string_view path)
{
    std::size_t const dot = path.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    std::string ext(path.substr(dot + 1));
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    if (ext == "ilbm" || ext == "iff" || ext == "lbm") {
        return "ilbm.datatype";
    }
    if (ext == "png") {
        return "png.datatype";
    }
    return {};
}

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

/* The candidates to ask, in order: the hint first (the extension may choose
 * which class to ask first), then every class file `DataTypes:` holds, sorted,
 * minus the hint. The list is the directory (specs/datatypes.md): a class is a
 * file, so dropping one in teaches the session a format with no registry to
 * update. An empty hint (a name that names no class) is the content-first case
 * -- every class is asked and one claims by content. */
std::vector<std::string> class_candidates(std::string const &hint)
{
    std::vector<std::string> names;
    if (!hint.empty()) {
        names.push_back(hint);
    }
    std::error_code error;
    std::filesystem::directory_iterator it("DataTypes:", error);
    if (error) {
        return names;
    }
    std::vector<std::string> found;
    std::filesystem::directory_iterator const end;
    for (; it != end; it.increment(error)) {
        if (error) {
            break;
        }
        std::error_code kind_error;
        if (it->is_directory(kind_error)) {
            continue;
        }
        std::string const name = it->path().filename().string();
        if (!name.empty()) {
            found.push_back(name);
        }
    }
    std::sort(found.begin(), found.end());
    for (std::string const &name : found) {
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    }
    return names;
}

/* Resolve a class `name` to a program path (specs/datatypes.md): the caller's
 * **program directory** first, `DataTypes:` under it. A class shipped beside a
 * program's binary is found whatever the current directory is -- the same order
 * and reason as `LIBS:` (specs/libraries.md). The caller's program directory is
 * its own, which is why the broker cannot work it out and the request carries
 * it. */
std::string class_path(std::string const &program_dir, std::string const &name)
{
    if (!program_dir.empty()) {
        std::string candidate(program_dir);
        if (candidate.back() != ':' && candidate.back() != '/') {
            candidate.push_back('/');
        }
        candidate.append(name);
        std::error_code error;
        if (std::filesystem::exists(candidate, error) && !error) {
            return candidate;
        }
    }
    std::string fallback("DataTypes:");
    fallback.append(name);
    return fallback;
}

/* Start the class at `path` under the caller's badge, serving the class port the
 * caller made (`class_cap`). `badge_out` is the class's, for a release if it
 * then declines the file. */
bool start_class(std::string const &path, seL4_CPtr class_cap, uint64_t *badge_out)
{
    return aegir::launch::spawn_serve(path.c_str(), static_cast<uint32_t>(path.size()),
                                      class_cap, badge_out);
}

/* Ask a started class to identify `path`: true when it claims the file. */
bool identify_class(seL4_CPtr class_cap, std::string const &path) noexcept
{
    aegir::ipc::Consumer const cls(class_cap);
    uint64_t request[aegir::ipc::kMaxWords] = {};
    uint32_t const words = aegir::nmspace::pack_string(
        request, path.data(), static_cast<uint32_t>(path.size()), aegir::nmspace::kPathMax);
    if (words == 0) {
        return false;
    }
    uint64_t answer[1] = {};
    aegir::ipc::WordsReply const reply =
        cls.call_words(aegir::datatypes::kMethodIdentify, request, words, answer, 1);
    return reply.error == 0 && reply.count >= 1 && answer[0] == 1;
}

/* Stop a class that declined the file: the launcher reaps it, so its capability
 * on the caller's port is gone and the next candidate can be started on the
 * same port without two classes serving it (specs/datatypes.md). A class that
 * claimed the file lives for the session. */
void release_class(uint64_t badge) noexcept
{
    aegir::ipc::Consumer const launcher = aegir::launch::launcher();
    if (!launcher.valid() || badge == 0) {
        return;
    }
    uint64_t answer[1] = {};
    (void)launcher.call_words(aegir::launch::kMethodRelease, &badge, 1, answer, 1);
}

void handle_open(uint64_t const *words, uint32_t count, bool cap_arrived, uint64_t *reply,
                 uint32_t *reply_count)
{
    reply[0] = 0;
    *reply_count = 1;
    /* The class port the caller made rides as the request's one capability. It
     * is picked up whatever happens next: a capability left in the scratch
     * receive slot is the next open's problem. */
    if (!cap_arrived) {
        return;
    }
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_class_slot,
                      aegir::bootstrap::cnode_bits());
    if (!aegir::ipc::take_received_cap(g_class_slot)) {
        return;
    }
    uint32_t at = 0;
    std::string class_name;
    std::string path;
    std::string program_dir;
    if (!read_string(words, count, at, class_name) ||
        !read_string(words, count, at, path) ||
        !read_string(words, count, at, program_dir)) {
        return;
    }
    if (class_name.empty()) {
        class_name = class_from_extension(path);
    }
    /* Content-first (specs/datatypes.md): ask the hint first, then walk
     * `DataTypes:`; the class, not the name, decides. Each name resolves through
     * the caller's program directory first, then `DataTypes:`. A class that
     * declines the file is released, so the next candidate is the only one
     * serving the caller's port. The cue names the path the class was loaded
     * from, so the program-directory half of the search is visible. */
    for (std::string const &candidate : class_candidates(class_name)) {
        std::string const resolved = class_path(program_dir, candidate);
        uint64_t badge = 0;
        if (!start_class(resolved, g_class_slot, &badge)) {
            continue;
        }
        if (identify_class(g_class_slot, path)) {
            aegir::debug_write("  datatypes: open ");
            aegir::debug_write(resolved.c_str());
            aegir::debug_write("\n");
            reply[0] = 1;
            break;
        }
        aegir::debug_write("  datatypes: ");
        aegir::debug_write(resolved.c_str());
        aegir::debug_write(" declines\n");
        release_class(badge);
    }
    if (reply[0] != 1) {
        aegir::debug_write("  datatypes: no class for ");
        aegir::debug_write(path.c_str());
        aegir::debug_write("\n");
    }
    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, g_class_slot,
                      aegir::bootstrap::cnode_bits());
}

/* The untyped, the VSpace root and the window the spawner gave this service, and
 * the heap over them (specs/cxx.md; the class service's stand-up is the same). */
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
    constexpr uint64_t kHeapBytes = 4ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        return false;
    }
    g_class_slot = g_objects.alloc_slot();
    return g_class_slot != 0;
}

} // namespace

int main()
{
    if (!adopt_memory()) {
        aegir::debug_write("  datatypes: FAIL no memory to stand up\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The endpoint auth made: the broker owns it. */
    aegir::ipc::Owner port = aegir::ipc::Owner::find(aegir::datatypes::kBrokerPortName,
                                                    aegir::datatypes::kBrokerPortNameLength);
    if (!port.valid()) {
        aegir::debug_write("  datatypes: FAIL no datatypes.main port to own\n");
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
        if (method == aegir::datatypes::kMethodOpen) {
            handle_open(words, count, cap_arrived, reply, &reply_count);
        } else if (method == aegir::datatypes::kMethodClose) {
            reply[0] = 1;
            reply_count = 1;
        } else {
            reply[0] = 0;
            reply_count = 1;
        }
        port.reply_words(reply, reply_count);
    }
}
