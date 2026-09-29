/*
 * aegir-launcher: the session's launcher (specs/launch.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One launcher per session: the service that holds the spawn kit -- the
 * untyped, the ASID pool, the unbadged console.gui, mem.main, the namespace,
 * the log, the clock and timer -- and serves launch.session. The shell's
 * commands, the bureau's Execute, and later a dock and the desktop icons are
 * its clients; none of them holds the kit itself (specs/launch.md). It is
 * headless: it never draws, so it links no toolkit.
 *
 * auth starts it with the session and makes its endpoint: the owner half
 * arrives as the port `launch.session`, and the bureau and the terminal are
 * handed caller halves of the same endpoint, so no client depends on a name
 * the launcher chose (specs/authority.md). A request carries the caller's
 * con.stream when it has one; a request with none starts a command whose
 * output goes to an output view (the next piece).
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/heap.h>
#include <aegir/launch.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/vspace.h>
#include <aegir/memory.h>
#include <aegir/nmspace.h>
#include <aegir/spawn/service_kit.h>
#include <sel4/sel4.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

void write(char const *text) noexcept
{
    aegir::debug_write("  launcher: ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

/* A command the launcher started: the spawn progress the acceptance cues on
 * (specs/launch.md). Only the spawner can say it, and the launcher is the
 * spawner now, so this is where the cue lives. */
void write_started(char const *name)
{
    std::string line("command started ");
    line.append(name);
    write(line.c_str());
}

/* Static, like every service's: an allocator carries the tables of what it
 * handed out, and a service's stack is pages. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);
/* The allocator's node pool: it splits the session's untyped into the
 * launcher's endpoints and the staging's page tables, and a service's stack is
 * pages, so the pool is a static region rather than the heap it serves -- the
 * heap cannot exist before the allocator that maps it. */
alignas(64) unsigned char g_nodes[64 * 1024];

/* Unpack the next string in the request, advancing `at` (the namespace
 * protocol's shape, aegir/nmspace.h). */
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

/* A nested terminal that gave no window gets this one, clear of the session
 * terminal's own and offset per nested child (specs/launch.md). */
std::string default_window(uint64_t index)
{
    int const x = 40 + 24 * static_cast<int>(index);
    int const y = 120 + 24 * static_cast<int>(index);
    return "CON:" + std::to_string(x) + "/" + std::to_string(y) + "/560/380/Terminal";
}

/* Start one program (kMethodSpawn). The request's fields are the wire's
 * (aegir/launch.h): kind, flags, argv, cwd, environment, path, std_in,
 * std_out, window, stack. `stream` is the caller's con.stream capability, or
 * zero when it sent none. */
void handle_spawn(aegir::spawn::ServiceKit &service, uint64_t const *words, uint32_t count,
                  bool cap_arrived, uint64_t *reply, uint32_t *reply_count)
{
    reply[0] = 0;
    *reply_count = 1;
    if (count < 2) {
        return;
    }
    uint32_t at = 0;
    uint64_t const kind = words[at++];
    uint64_t const flags = words[at++];
    std::string argv;
    std::string cwd;
    std::string environment;
    std::string path;
    std::string std_in;
    std::string std_out;
    std::string window;
    if (!read_string(words, count, at, argv) || !read_string(words, count, at, cwd) ||
        !read_string(words, count, at, environment) ||
        !read_string(words, count, at, path) || !read_string(words, count, at, std_in) ||
        !read_string(words, count, at, std_out) || !read_string(words, count, at, window) ||
        at >= count) {
        return;
    }
    uint64_t const stack_pages = words[at++];

    std::vector<std::string> const argv_words = aegir::spawn::ServiceKit::split_words(argv);
    if (argv_words.empty()) {
        return;
    }

    /* The caller's stream (specs/launch.md): a launcher's command writes where
     * its caller does. A capability that arrived is moved before anything else
     * can occupy the scratch slot. A caller with none (a launching program, or
     * any client that draws its own window) gets the launcher's read-only
     * output view, started once; the command's con.stream is a badged copy of
     * it. */
    seL4_CPtr stream = 0;
    bool stream_copy = false;
    if (cap_arrived) {
        if (!service.hold_received_stream()) {
            write("FAIL the caller's stream would not move");
            return;
        }
        stream = service.received_stream();
        stream_copy = true;
    } else {
        aegir::spawn::ServiceKit::Started view{};
        if (!service.start_output_view(&view)) {
            write("FAIL no output view for a stream-less command");
            return;
        }
        stream = service.output_stream();
    }

    if (kind == aegir::launch::kKindLaunching) {
        /* A launching peer (specs/launch.md): a nested terminal with its own
         * window, badge range and shell. The words after argv[0] (a
         * `NEWSHELL FROM <file>`) travel as the child's own arguments. */
        std::string const program = argv_words[0];
        std::vector<std::string> const arguments(argv_words.begin() + 1, argv_words.end());
        aegir::spawn::ServiceKit::Started started{};
        if (service.start_launcher(program, window, arguments, default_window(0), cwd,
                                   &started)) {
            write("nested terminal started");
            reply[0] = 1;
        }
        *reply_count = 1;
        return;
    }
    if (kind != aegir::launch::kKindCommand) {
        return;
    }
    aegir::spawn::ServiceKit::Command command;
    command.words = &argv_words;
    command.cwd = &cwd;
    command.environment = &environment;
    command.path = &path;
    command.std_in = &std_in;
    command.std_out = &std_out;
    command.background = (flags & aegir::launch::kFlagBackground) != 0;
    command.stack_pages = static_cast<uint32_t>(stack_pages);
    /* The cap is already badged (the caller minted it with the stream key), so
     * the command takes a copy rather than a mint; a view's endpoint is the
     * launcher's own, so the command's copy is minted with its badge. */
    command.stream = stream;
    command.stream_copy = stream_copy;
    aegir::spawn::ServiceKit::Started started{};
    if (!service.start_command(command, &started)) {
        return;
    }
    write_started(argv_words[0].c_str());
    reply[0] = 1;
    reply[1] = started.badge;
    *reply_count = 2;
}

/* Start a line's stages (kMethodPipeline): a stage count, each stage's argv
 * and its own redirections, then the context once (specs/pipe.md). */
void handle_pipeline(aegir::spawn::ServiceKit &service, uint64_t const *words,
                     uint32_t count, bool cap_arrived, uint64_t *reply,
                     uint32_t *reply_count)
{
    reply[0] = 0;
    *reply_count = 1;
    if (count < 3) {
        return;
    }
    uint32_t at = 0;
    uint64_t const kind = words[at++];
    uint64_t const flags = words[at++];
    uint32_t const stage_count = static_cast<uint32_t>(words[at++]);
    if (kind != aegir::launch::kKindCommand || stage_count == 0 || stage_count > count) {
        return;
    }
    std::vector<aegir::spawn::ServiceKit::Stage> stages;
    stages.reserve(stage_count);
    for (uint32_t i = 0; i < stage_count; ++i) {
        aegir::spawn::ServiceKit::Stage stage;
        if (!read_string(words, count, at, stage.line) ||
            !read_string(words, count, at, stage.std_in) ||
            !read_string(words, count, at, stage.std_out)) {
            return;
        }
        stages.push_back(std::move(stage));
    }
    std::string cwd;
    std::string environment;
    std::string path;
    if (!read_string(words, count, at, cwd) || !read_string(words, count, at, environment) ||
        !read_string(words, count, at, path) || at >= count) {
        return;
    }
    uint64_t const stack_pages = words[at++];

    seL4_CPtr stream = 0;
    bool stream_copy = false;
    if (cap_arrived) {
        if (!service.hold_received_stream()) {
            write("FAIL the caller's stream would not move");
            return;
        }
        stream = service.received_stream();
        stream_copy = true;
    } else {
        aegir::spawn::ServiceKit::Started view{};
        if (!service.start_output_view(&view)) {
            write("FAIL no output view for a stream-less pipeline");
            return;
        }
        stream = service.output_stream();
    }

    aegir::spawn::ServiceKit::Command context;
    context.cwd = &cwd;
    context.environment = &environment;
    context.path = &path;
    context.background = (flags & aegir::launch::kFlagBackground) != 0;
    context.stack_pages = static_cast<uint32_t>(stack_pages);
    context.stream = stream;
    context.stream_copy = stream_copy;
    /* The reply carries the stages' badges after the answer, up to the
     * envelope's ceiling; a line with more stages than that still runs (the
     * caller learns the rest from their exits). */
    uint32_t const room = aegir::ipc::kMaxWords > 1 ? aegir::ipc::kMaxWords - 1 : 0;
    uint32_t const cap = stage_count < room ? stage_count : room;
    std::vector<aegir::spawn::ServiceKit::Started> started(cap);
    if (!service.start_pipeline(stages.data(), stage_count, context, started.data(), cap)) {
        return;
    }
    for (uint32_t i = 0; i < stage_count; ++i) {
        std::vector<std::string> const words =
            aegir::spawn::ServiceKit::split_words(stages[i].line);
        if (!words.empty()) {
            write_started(words[0].c_str());
        }
    }
    reply[0] = 1;
    for (uint32_t i = 0; i < cap; ++i) {
        reply[1 + i] = started[i].badge;
    }
    *reply_count = 1 + cap;
}

/* Take a command back (kMethodRelease): its stream has seen it exit. */
void handle_release(aegir::spawn::ServiceKit &service, uint64_t const *words, uint32_t count,
                    uint64_t *reply, uint32_t *reply_count)
{
    reply[0] = 0;
    *reply_count = 1;
    if (count < 1) {
        return;
    }
    reply[0] = static_cast<uint64_t>(service.release(words[0]));
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    /* The endpoint auth made: the owner half of launch.session. */
    aegir::ipc::Owner port = aegir::ipc::Owner::find(aegir::launch::kPortName,
                                                    aegir::launch::kPortNameLength);
    if (!port.valid()) {
        write("FAIL no launch.session port to own");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The allocator's node pool, before it splits anything: the pool is what
     * lets the 22-bit untyped split all the way down to a notification. */
    g_objects.adopt_nodes(g_nodes, sizeof(g_nodes));

    /* The launcher's own memory: the untyped its objects and its children's
     * staging come from, and the window it maps the staging through. */
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
    /* One CSpace, two cursors (specs/direction.md): the launcher's own objects
     * come up from the block's slots, the commands' pool down from the top, so
     * a slot cannot be handed out twice while anything is left and no boundary
     * between them has to be guessed at. The pool is owner-tagged, so a
     * command's capabilities can be revoked and its slots returned whole
     * (specs/memory.md Phase 5). */
    uint64_t const total_slots = 1ull << aegir::bootstrap::cnode_bits();
    g_objects.adopt_slots(first_free, total_slots - first_free, 0);
    if (!g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                         static_cast<uintptr_t>(window_base),
                         static_cast<uintptr_t>(window_base + window_bytes), &g_objects)) {
        write("FAIL the window I was given could not be adopted");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    /* The C++ heap (specs/cxx.md): the request handler builds std::string and
     * std::vector, and a nested terminal's image is read into one, so the
     * launcher needs a heap of its own -- the window's tail, as every hosted
     * process claims it. */
    constexpr uint64_t kHeapBytes = 8ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        write("FAIL the heap would not claim the window");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The spawn kit, adopted by name from the block auth installed, over the
     * command pool -- the whole CSpace, handed out from the top. */
    aegir::spawn::ServiceKit service;
    if (!service.adopt(g_objects, g_scratch, first_free,
                       static_cast<uint32_t>(total_slots - first_free), true)) {
        write("FAIL the spawn kit was not given");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    /* The badges this session's commands are owned by: the range auth delegated
     * (specs/launch.md), so a command's memory is charged to the user and the
     * limits apply (specs/memory.md). */
    uint64_t own_badge = 0;
    if (!aegir::bootstrap::badge(&own_badge)) {
        own_badge = 0;
    }
    service.adopt_identity(own_badge);

    write("ready, serving launch.session");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    /* The launch protocol (aegir/launch.h): the launcher's own endpoint, so a
     * call and a reply is all it does. */
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
        if (method == aegir::launch::kMethodSpawn) {
            handle_spawn(service, words, count, cap_arrived, reply, &reply_count);
        } else if (method == aegir::launch::kMethodPipeline) {
            handle_pipeline(service, words, count, cap_arrived, reply, &reply_count);
        } else if (method == aegir::launch::kMethodRelease) {
            handle_release(service, words, count, reply, &reply_count);
        } else {
            reply[0] = 0;
            reply_count = 1;
        }
        port.reply_words(reply, reply_count);
    }
}
