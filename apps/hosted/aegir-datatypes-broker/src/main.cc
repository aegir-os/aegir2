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
#include <string>
#include <string_view>

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

/* The class from a file's extension -- the first cut's hint (the client's own
 * mapping, and a content-sniffing broker is the generalization). */
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

/* Start `class_name` under the caller's badge, serving the class port the
 * caller made (`class_cap`), and ask it to identify `path`. True when it claims
 * the file. The class is left running: it lives for the session, and the caller
 * talks to the port it made. */
bool start_and_identify(std::string const &class_name, std::string const &path,
                        seL4_CPtr class_cap)
{
    std::string program("DataTypes:");
    program.append(class_name);
    uint64_t badge = 0;
    if (!aegir::launch::spawn_serve(program.c_str(),
                                    static_cast<uint32_t>(program.size()), class_cap,
                                    &badge)) {
        return false;
    }
    static_cast<void>(badge);

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
    if (!class_name.empty()) {
        aegir::debug_write("  datatypes: open ");
        aegir::debug_write(class_name.c_str());
        aegir::debug_write("\n");
        reply[0] = start_and_identify(class_name, path, g_class_slot) ? 1 : 0;
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
