/*
 * aegir-terminal: the session's CON: handler.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The terminal owns one console window and a text grid, and serves con.stream
 * on its own endpoint. The shell is its own process now (aegir-shell): it
 * opens a stream, reads lines, runs the built-ins, and asks the terminal to
 * run a command. The terminal holds the window and the spawn authority, so a
 * command starts here -- with a caller copy of the shell's stream, so its
 * output lands on the same grid -- and its exit is read back by the shell
 * (specs/terminal.md, specs/shell.md, specs/authority.md).
 */

#include "spawn_kit.h"
#include "console_stream_server.h"

#include <aegir/bootstrap.h>
#include <aegir/clock.h>
#include <aegir/console.h>
#include <aegir/console_stream.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/nmspace.h>
#include <aegir/spawn/process.h>
#include <aegir/timer.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/line_editor.h>
#include <aegir/trinket/terminal_view.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/unicode.h>
#include <aegir/trinket/window.h>
#include <sel4/sel4.h>

#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

/* The shell's stream, keyed by this number: the shell's con.stream copy is
 * badged with it, and so is every command's, so all of them share one stream
 * (specs/terminal.md). Not a badge the kernel minted, just a key inside the
 * handler. */
constexpr uint64_t kShellStream = 1;

/* Clear of the test bed's windows, the demo, and the screen bar's samples. */
constexpr int kWindowX = 40;
constexpr int kWindowY = 120;
constexpr int kWindowWidth = 560;
constexpr int kWindowHeight = 380;

void write(char const *text)
{
    aegir::debug_write(text);
}

void write_unsigned(uint64_t value)
{
    aegir::debug_write_unsigned(value);
}

std::vector<std::string> split_words(std::string const &text)
{
    std::vector<std::string> words;
    std::string current;
    for (char const c : text) {
        if (c == ' ' || c == '\t') {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        words.push_back(current);
    }
    return words;
}

/* The packed words a string of `length` bytes occupies: the count word, then
 * the bytes (nmspace::pack_string's own arithmetic). */
uint32_t packed_words(uint32_t length)
{
    return 1 + (length + 7) / 8;
}

}  // namespace

int main(int argc, char *argv[])
{
    using namespace aegir::trinket;

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    Application& app = Application::create(argc, argv);

    /* The authority to start the session's commands and its shell
     * (specs/authority.md, specs/shell.md): auth delegates it, the terminal
     * adopts it. A failure here leaves the window but no command line. */
    aegir::terminal::SpawnKit spawn_kit;
    bool const kit = spawn_kit.adopt(app);
    if (kit) {
        write("  terminal: spawn kit ready\n");
    } else {
        write("  terminal: no spawn kit -- no shell, no commands\n");
    }

    aegir::ipc::Consumer const gui = aegir::ipc::Consumer::find(
        aegir::console::kPortName, aegir::console::kPortNameLength);
    if (!gui.valid()) {
        write("  terminal: FAIL no console.gui\n");
        return 1;
    }
    app.set_gui_port(gui);

    Window window(app);
    window.set_title("Terminal");
    window.set_rect({kWindowX, kWindowY, kWindowWidth, kWindowHeight});

    auto view = std::make_unique<TerminalView>();
    view->set_font(app.default_font());
    view->set_colors(app.theme().color(ColorRole::TEXT),
                     app.theme().color(ColorRole::WINDOW_BG));
    TerminalView *const terminal = view.get();

    aegir::terminal::ConsoleStreamServer server(terminal->buffer());
    /* The grid changed -- the banner, a command's output, the next prompt --
     * so the view that draws it must repaint. */
    server.on_change = [terminal]() { terminal->damage(); };

    /* The terminal's keys, routed by the handler: the shell's editor while it
     * is idle, the stream's input queue while a command runs (specs/shell.md's
     * Phase 4). The shell is its own process and opens the stream a moment
     * after the terminal says it is ready, so a key that arrives before its
     * editor exists waits here and is replayed (on_poll). */
    std::vector<KeyEvent> pending_keys;
    terminal->on_key = [&server, &pending_keys](KeyEvent const &event) {
        LineEditor *const editor = server.editor(kShellStream);
        bool const ready =
            editor != nullptr && (editor->editing() || server.in_command(kShellStream));
        /* Hold the key when the shell has not begun its prompt yet (or is
         * between commands), and also when earlier keys are still held: a key
         * fed straight to the editor would otherwise overtake one on_poll has
         * not replayed, and the line would read out of order. on_poll replays
         * the held keys, in order, once the editor is ready. */
        if (!ready || !pending_keys.empty()) {
            pending_keys.push_back(event);
            if (server.on_wake) {
                server.on_wake(kShellStream);
            }
            return true;
        }
        return server.on_key(kShellStream, event);
    };

    /* One buffer for every image the terminal reads, kept across commands: its
     * pages come from the toolkit's untyped, and the hosted heap never returns
     * a large mapping (sys_munmap is a no-op, specs/cxx.md), so a fresh vector
     * per command would spend the untyped a command at a time. It is sized
     * once from the file, so the grow-by-doubling that leaves a trail of
     * mappings never happens. */
    std::vector<char> image;
    auto load_image = [&](std::string const &path) -> bool {
        int const fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }
        image.clear();
        /* The size comes from the fd already open, not a second resolve by
         * path: the fd holds the volume capability, and a path stat would ask
         * the namespace to resolve the same file again. (A terminal that did
         * the path stat hung here; the fd's own stat is both fewer calls and
         * the one that cannot disagree with the fd the bytes come from.) */
        struct stat info {};
        if (::fstat(fd, &info) == 0 && info.st_size > 0) {
            image.reserve(static_cast<std::size_t>(info.st_size));
        }
        char chunk[512];
        ssize_t have = 0;
        while ((have = ::read(fd, chunk, sizeof(chunk))) > 0) {
            image.insert(image.end(), chunk, chunk + have);
        }
        ::close(fd);
        return !image.empty();
    };

    uint64_t command_serial = 0;
    aegir::spawn::Process command_process{};
    bool command_running = false;
    auto spawn_command = [&](std::string const &name, std::vector<std::string> const &args,
                             std::string const &cwd,
                             std::vector<char const *> const &environment,
                             std::string const &std_in, std::string const &std_out) -> bool {
        if (!kit) {
            return false;
        }
        /* The image comes from C: -- the alias of Sys:C, the command set
         * (specs/dos.md) -- through the namespace, not from a mapped initrd. */
        if (!load_image("C:" + name)) {
            write("  terminal: no image for the command\n");
            return false;
        }

        /* The command pool: a fresh bracket, so this command's objects and the
         * untyped its runtime gets are reclaimed whole when it exits
         * (specs/shell.md's Phase 4). */
        if (!spawn_kit.begin()) {
            write("  terminal: FAIL the command pool would not adopt\n");
            return false;
        }
        aegir::mem::Account account{"command", 0, 0, 0};
        seL4_Error untyped_error = seL4_NoError;
        uint64_t command_untyped_physical = 0;
        seL4_CPtr const command_untyped = spawn_kit.memory().carve_untyped(
            aegir::terminal::SpawnKit::kCommandUntypedBits, account, &untyped_error,
            &command_untyped_physical);
        if (command_untyped == 0) {
            write("  terminal: FAIL no untyped for the command's runtime\n");
            spawn_kit.abort();
            return false;
        }

        /* `Request.arguments` is what follows argv[0]: the spawner writes
         * `request.name` as argv[0] itself (specs/environment.md). */
        std::vector<char const *> argument_pointers;
        for (std::string const &arg : args) {
            argument_pointers.push_back(arg.c_str());
        }
        static std::string const kAccountText = "command";
        aegir::spawn::PortGrant ports[6] = {
            {aegir::console::kStreamPortName, aegir::console::kStreamPortNameLength,
             aegir::bootstrap::kSlotFirstDeclared, spawn_kit.stream_endpoint(),
             seL4_CapRights_new(1, 1, 0, 1), kShellStream, 0},
            /* The command's own runtime kit: the untyped its heap and page
             * tables come from, and (below) its own VSpace root and window. */
            {"untyped", 7, aegir::bootstrap::kSlotFirstDeclared + 1, command_untyped,
             seL4_AllRights, 0, aegir::terminal::SpawnKit::kCommandUntypedBits},
            /* The namespace the command needs, copied from the terminal's own
             * badged cap: the copy keeps the session's identity, so the command
             * resolves Home:/ENV:/C: and its writes are owned by the session
             * (specs/dos.md). Copied, not minted: a badged endpoint cap cannot
             * be minted again (specs/authority.md). */
            {aegir::nmspace::kPortName, aegir::nmspace::kPortNameLength,
             aegir::bootstrap::kSlotFirstDeclared + 2, spawn_kit.command_nmspace_port(),
             seL4_CapRights_new(1, 1, 0, 1), 0, 0, false, true},
            /* The command's console doorbell: the notification the terminal
             * rings when this command's stream has input, so the command's
             * `read` parks on it (specs/terminal.md). A second entry in the
             * stream's set, beside the shell's own. */
            {aegir::console::kDoorbellName, aegir::console::kDoorbellNameLength,
             aegir::bootstrap::kSlotFirstDeclared + 3, spawn_kit.command_doorbell(),
             seL4_AllRights, 0, 0, false, true},
        };
        uint32_t port_count = 4;
        if (spawn_kit.command_clock_port() != 0) {
            /* The clock, minted from the terminal's unbadged copy: the tools
             * ask the time through the runtime's clock_gettime (specs/dos.md). */
            ports[port_count] = {aegir::clock::kPortName, aegir::clock::kPortNameLength,
                                 aegir::bootstrap::kSlotFirstDeclared + port_count,
                                 spawn_kit.command_clock_port(),
                                 seL4_CapRights_new(1, 0, 0, 1), 0, 0};
            ++port_count;
        }
        if (spawn_kit.command_timer_port() != 0) {
            /* The timer, the interval side: the runtime's nanosleep and
             * CLOCK_MONOTONIC answer through it (specs/timer.md). */
            ports[port_count] = {aegir::timer::kPortName, aegir::timer::kPortNameLength,
                                 aegir::bootstrap::kSlotFirstDeclared + port_count,
                                 spawn_kit.command_timer_port(),
                                 seL4_CapRights_new(1, 0, 0, 1), 0, 0};
            ++port_count;
        }
        aegir::spawn::Request request{};
        request.name = name.c_str();
        request.name_length = static_cast<uint32_t>(name.size());
        request.binary_image = image.data();
        request.binary_image_bytes = image.size();
        request.account = kAccountText.c_str();
        request.account_length = static_cast<uint32_t>(kAccountText.size());
        request.arguments = argument_pointers.data();
        request.argument_count = static_cast<uint32_t>(args.size());
        /* Inheritance: the command gets the environment the shell sent in its
         * `run`, so `Set` reaches it (specs/environment.md). */
        request.environment = environment.empty() ? nullptr : environment.data();
        request.environment_count = static_cast<uint32_t>(environment.size());
        request.cwd = cwd.c_str();
        request.cwd_length = static_cast<uint32_t>(cwd.size());
        /* The command's redirection (specs/shell.md): the shell parsed it off
         * the line, and the runtime routes the command's fd 0/1 to these paths
         * instead of the console stream. Empty is the console. */
        request.std_in = std_in.empty() ? nullptr : std_in.c_str();
        request.std_in_length = static_cast<uint32_t>(std_in.size());
        request.std_out = std_out.empty() ? nullptr : std_out.c_str();
        request.std_out_length = static_cast<uint32_t>(std_out.size());
        request.priority = seL4_MaxPrio - 2;
        request.ports = ports;
        request.port_count = port_count;
        request.fault_endpoint = spawn_kit.fault_endpoint();
        request.badge = 0x1000 + command_serial++;
        request.give_vspace = true;
        request.untyped_physical = command_untyped_physical;
        request.untyped_bits = aegir::terminal::SpawnKit::kCommandUntypedBits;

        if (!spawn_kit.spawner().spawn(request, account, command_process)) {
            write("  terminal: FAIL spawning a command: ");
            write(spawn_kit.spawner().problem());
            write("\n");
            spawn_kit.abort();
            return false;
        }
        command_running = true;
        /* The command inherits the shell's stream, and from here until it
         * exits the terminal routes the keyboard to that stream's input queue
         * rather than the idle editor -- the command's stdin. */
        server.begin_command(kShellStream);
        write("  terminal: command started ");
        write(name.c_str());
        write("\n");
        return true;
    };

    /* The shell: its own process, spawned once from its own pool. Its
     * con.stream copy is badged with the shell's stream, and it opens the
     * stream itself -- the terminal is serving before it gets there. It is
     * spawned in on_started, after the ready cue, so the cue is not delayed by
     * the spawn (the acceptance types at it before the demo's zoom). */
    if (kit) {
        app.serve(aegir::ipc::Owner(spawn_kit.stream_endpoint()));
        app.on_call = [&](uint32_t method, uint64_t const *words, uint32_t count,
                          seL4_Word badge, bool cap_arrived, uint64_t *reply,
                          uint32_t capacity) -> uint32_t {
            /* The shell asks the terminal to run a command: the line, the
             * directory to run it in, the shell's environment, and the
             * command's redirected input and output (five strings, packed in
             * that order; specs/shell.md). The terminal holds the spawn
             * authority, so it starts the command here. */
            if (method == aegir::console::kStreamMethodRun) {
                if (capacity < 1) {
                    return 0;
                }
                char const *line = nullptr;
                char const *cwd = nullptr;
                char const *environment = nullptr;
                char const *std_in = nullptr;
                char const *std_out = nullptr;
                uint32_t line_length = 0;
                uint32_t cwd_length = 0;
                uint32_t environment_length = 0;
                uint32_t std_in_length = 0;
                uint32_t std_out_length = 0;
                uint32_t at = 0;
                if (!aegir::nmspace::unpack_string(words + at, count - at,
                                                   aegir::console::kStreamBytesMax, &line,
                                                   &line_length)) {
                    return 0;
                }
                at += packed_words(line_length);
                if (!aegir::nmspace::unpack_string(words + at, count - at,
                                                   aegir::console::kStreamBytesMax, &cwd,
                                                   &cwd_length)) {
                    return 0;
                }
                at += packed_words(cwd_length);
                if (!aegir::nmspace::unpack_string(words + at, count - at,
                                                   aegir::console::kStreamBytesMax,
                                                   &environment, &environment_length)) {
                    return 0;
                }
                at += packed_words(environment_length);
                if (!aegir::nmspace::unpack_string(words + at, count - at,
                                                   aegir::console::kStreamBytesMax, &std_in,
                                                   &std_in_length)) {
                    return 0;
                }
                at += packed_words(std_in_length);
                if (!aegir::nmspace::unpack_string(words + at, count - at,
                                                   aegir::console::kStreamBytesMax, &std_out,
                                                   &std_out_length)) {
                    return 0;
                }
                std::vector<std::string> const words_of_line =
                    split_words(std::string(line, line_length));
                if (words_of_line.empty()) {
                    reply[0] = 0;
                    return 1;
                }
                /* The environment rides as NUL-separated NAME=VALUE; the
                 * spawner wants pointers, so they point into a copy. */
                std::vector<char> environment_buffer(environment,
                                                     environment + environment_length);
                std::vector<char const *> environment_pointers;
                std::size_t index = 0;
                while (index < environment_buffer.size()) {
                    environment_pointers.push_back(&environment_buffer[index]);
                    while (index < environment_buffer.size() &&
                           environment_buffer[index] != '\0') {
                        ++index;
                    }
                    ++index;
                }
                reply[0] = spawn_command(
                               words_of_line[0],
                               std::vector<std::string>(words_of_line.begin() + 1,
                                                        words_of_line.end()),
                               std::string(cwd, cwd_length), environment_pointers,
                               std::string(std_in, std_in_length),
                               std::string(std_out, std_out_length))
                               ? 1
                               : 0;
                return 1;
            }

            uint32_t const answer =
                server.handle(method, words, count, badge, reply, capacity);
            if (cap_arrived) {
                /* A capability rode with the call -- the shell's doorbell on
                 * its open (specs/terminal.md). Move it out of the scratch slot
                 * before the next receive, and record it for the stream. */
                seL4_CPtr const slot = app.alloc_slot();
                if (slot != 0 && aegir::ipc::take_received_cap(slot) &&
                    method == aegir::console::kStreamMethodOpen && answer == 1 &&
                    reply[0] == 1) {
                    server.set_doorbell(badge, slot);
                }
            }
            if (method == aegir::console::kStreamMethodCommandStatus && answer == 1) {
                /* The shell has taken the command's status, so the command is
                 * done: stop and reclaim it, so its pool goes back whole. */
                write("  terminal: command exited ");
                write_unsigned(reply[0]);
                write("\n");
                if (command_running) {
                    spawn_kit.finish(command_process.tcb);
                    command_running = false;
                }
            }
            return answer;
        };
        /* The handler says the stream has something to read; the terminal rings
         * the client's doorbell. The handler is a pure value, so the signal
         * lives here. A running command's `read` parks on the command doorbell
         * instead of its own, so ring that too (specs/terminal.md). */
        server.on_wake = [&](uint64_t caller) {
            seL4_CPtr const slot = server.doorbell(caller);
            if (slot != 0) {
                seL4_Signal(slot);
            }
            if (kit && server.in_command(caller) && spawn_kit.command_doorbell() != 0) {
                seL4_Signal(spawn_kit.command_doorbell());
            }
        };
    }

    window.set_content(std::move(view));
    window.set_focus(terminal);
    /* The terminal starts focused, so the acceptance's typed line reaches it
     * without a click that would race the demo's. */
    window.request_focus();
    window.show();

    /* The shell opens its stream a moment after the terminal is up; keys that
     * arrived first are replayed once its editor is there. */
    app.on_poll = [&]() {
        if (pending_keys.empty()) {
            return;
        }
        LineEditor *const editor = server.editor(kShellStream);
        if (editor == nullptr || (!editor->editing() && !server.in_command(kShellStream))) {
            return;
        }
        std::vector<KeyEvent> keys;
        keys.swap(pending_keys);
        for (KeyEvent const &event : keys) {
            (void)server.on_key(kShellStream, event);
        }
    };

    app.on_started = [&]() {
        write("  terminal: ready\n");
        if (kit) {
            std::error_code cwd_error;
            std::string const cwd = std::filesystem::current_path(cwd_error).string();
            if (!load_image("Initrd:aegir-shell")) {
                write("  terminal: no image for the shell\n");
            } else if (!spawn_kit.spawn_shell(image.data(), image.size(), cwd.c_str(),
                                              static_cast<uint32_t>(cwd.size()),
                                              kShellStream)) {
                write("  terminal: FAIL the shell would not start\n");
            } else {
                write("  terminal: shell started\n");
            }
        }
        if (log.valid()) {
            (void)log.call(aegir::log::kMethodEvent,
                           static_cast<uint64_t>(aegir::log::Event::Ready));
        }
        /* No supervision signal: this session is long-lived, like the bureau
         * (specs/workbench.md). */
    };

    return app.exec();
}