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
#include <aegir/environment.h>
#include <aegir/ipc/port.h>
#include <aegir/launch.h>
#include <aegir/log.h>
#include <aegir/memory.h>
#include <aegir/nmspace.h>
#include <aegir/spawn/process.h>
#include <aegir/timer.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/line_editor.h>
#include <aegir/trinket/terminal_view.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/unicode.h>
#include <aegir/trinket/window.h>
#include <aegir/trinket/window_spec.h>
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

/* The shell's command words. The shell has already substituted variables and
 * grouped quotes, so it sends the words NUL-separated and the terminal does
 * not re-lex them: the first word is the command, the rest its arguments
 * (specs/shell.md). */
std::vector<std::string> split_command_words(std::string const &text)
{
    std::vector<std::string> words;
    if (text.empty()) {
        return words;
    }
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\0') {
            words.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return words;
}

/* The packed words a string of `length` bytes occupies: the count word, then
 * the bytes (nmspace::pack_string's own arithmetic). */
uint32_t packed_words(uint32_t length)
{
    return 1 + (length + 7) / 8;
}

/* Parse a `AEGIR_BADGE_RANGE=<base>,<size>` value (specs/launch.md). True and
 * fills base/size when it is well formed; a malformed value is refused rather
 * than guessed, so a terminal is never handed someone else's serials. */
bool parse_badge_range(char const *value, uint64_t *base, uint64_t *size)
{
    if (value == nullptr) {
        return false;
    }
    uint64_t values[2] = {0, 0};
    uint32_t which = 0;
    bool any_digit = false;
    for (char const *p = value;; ++p) {
        char const c = *p;
        if (c >= '0' && c <= '9') {
            values[which] = values[which] * 10 + static_cast<uint64_t>(c - '0');
            any_digit = true;
        } else if (c == ',' && which == 0) {
            if (!any_digit) {
                return false;
            }
            which = 1;
            any_digit = false;
        } else if (c == '\0') {
            if (!any_digit || which != 1 || values[1] == 0) {
                return false;
            }
            *base = values[0];
            *size = values[1];
            return true;
        } else {
            return false;
        }
    }
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
    /* The titlebar gadgets (specs/window-manager.md): close and zoom, with the
     * depth gadget a decorated window carries by default. Closing the terminal
     * closes the session's console -- the shell's stream loses its owner -- so
     * it is the Amiga's `close the Shell window`, made literal. */
    window.set_gadgets(true, true, true);
    window.on_close_requested = [&app]() { app.quit(0); };
    /* A launcher sets AEGIR_WINDOW (specs/launch.md): this terminal is nested,
     * and the specification is its window. The terminal owns the window, so it
     * is the one that parses it; the launcher only forwards it. */
    bool nested = false;
    {
        char const *const spec = aegir::environment::getenv("AEGIR_WINDOW");
        if (spec != nullptr) {
            nested = true;
            std::string title;
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            if (parse_window_spec(spec, &x, &y, &width, &height, &title)) {
                window.set_rect({x, y, width, height});
                if (!title.empty()) {
                    window.set_title(title.c_str());
                }
            }
            write("  terminal: nested window ");
            write(spec);
            write("\n");
        }
    }

    /* A launcher's `FROM <file>` (specs/launch.md) rides as this program's own
     * arguments: the shell this terminal starts runs the named file instead of
     * Shell-Startup. */
    std::string startup_file;
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] != nullptr && std::string(argv[i]) == "FROM" && argv[i + 1] != nullptr) {
            startup_file = argv[i + 1];
            break;
        }
    }

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
    /* The boot session's failure view (specs/boot.md): once the shell has said
     * the sequence failed, the window is up and takes no input. The cue is
     * printed on the next poll, after the window has repainted, so a screendump
     * cued by it sees the view rather than the frame before it. */
    bool read_only = false;
    bool announce_view = false;
    terminal->on_key = [&server, &pending_keys, &read_only](KeyEvent const &event) {
        if (read_only) {
            return false;
        }
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
    uint64_t pipeline_serial = 0;
    /* One owner per live command; the slot pool's 0 means free, so owners run
     * from 1. */
    uint64_t owner_serial = 0;
    /* This terminal's own badge (specs/shell.md's interim, now landed): a
     * session terminal is a user process, so its commands' memory is owned by
     * a user badge and limits apply (specs/memory.md). The boot terminal is a
     * system process, so its commands stay unlimited like it is. */
    uint64_t own_badge = 0;
    if (!aegir::bootstrap::badge(&own_badge)) {
        own_badge = 0;
    }
    /* The badge range this terminal hands out (specs/launch.md): auth sets it
     * for a session terminal, a launcher sets it for a nested one. Commands
     * and nested terminals draw serials from it, so no two of a session's
     * processes share one; a system terminal has none and its commands keep
     * the small system badges. A quarter is reserved for nested terminals. */
    uint64_t badge_base = 0;
    uint64_t badge_size = 0;
    bool const have_badge_range =
        parse_badge_range(aegir::environment::getenv("AEGIR_BADGE_RANGE"), &badge_base,
                          &badge_size);
    uint64_t next_command = badge_base;
    uint64_t child_count = 0;
    uint64_t const child_size = badge_size / 4;
    /* The live commands: each owns a pool slot range (`owner`) and its memory
     * chunks (`badge`), and a background command is just another of them
     * (specs/memory.md Phase 5). A foreground line's records are reaped when
     * the shell takes its status; a background command's when its exit
     * arrives. */
    struct LiveCommand {
        aegir::spawn::Process process;
        uint64_t badge = 0;
        uint32_t owner = 0;
        bool foreground = false;
    };
    std::vector<LiveCommand> live;
    uint32_t foreground_outstanding = 0;
    /* The stages of the foreground line now running: >1 makes it a pipeline,
     * which the completion cue names apart from a single command
     * (specs/pipe.md). */
    uint32_t command_stage_count = 0;
    auto spawn_one = [&](std::string const &name, std::vector<std::string> const &args,
                         std::string const &cwd,
                         std::vector<char const *> const &environment,
                         std::string const &std_in, std::string const &std_out,
                         bool background, uint32_t stack_pages) -> bool {
        if (!kit) {
            return false;
        }
        /* The image comes from C: -- the alias of Sys:C, the command set
         * (specs/dos.md) -- through the namespace, not from a mapped initrd.
         * The spawner copies the image into the child before it returns, so
         * the one buffer is reused for the next stage. */
        if (!load_image("C:" + name)) {
            write("  terminal: no image for the command ");
            write(name.c_str());
            write("\n");
            return false;
        }

        /* The command's id: its stream badge (a key inside the terminal, not a
         * kernel badge) and its memory owner. A session terminal mints a user
         * badge from the range it was granted (specs/launch.md), so the memory
         * service resolves the command's class and limits (specs/memory.md,
         * specs/limits.md) and a nested terminal's commands never share a
         * serial with its parent's; a system terminal's commands stay system
         * badges, unlimited. A range with none left refuses rather than
         * reusing a serial. */
        uint64_t command_badge = 0;
        if (aegir::ipc::is_user_badge(own_badge)) {
            uint64_t const floor = have_badge_range
                                       ? badge_base + badge_size - 3 * child_size
                                       : 0;
            if (have_badge_range && next_command >= floor) {
                write("  terminal: FAIL the badge range is spent\n");
                return false;
            }
            command_badge = aegir::ipc::make_user_badge(
                aegir::ipc::user_index(own_badge), next_command++);
        } else {
            command_badge = 0x1000 + command_serial++;
        }
        uint32_t const owner = static_cast<uint32_t>(++owner_serial);
        if (!spawn_kit.begin(owner)) {
            write("  terminal: FAIL the command's staging would not begin\n");
            return false;
        }
        if (!spawn_kit.begin_command(command_badge)) {
            write("  terminal: FAIL no memory copy for the command\n");
            spawn_kit.abandon(command_badge, owner);
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
            spawn_kit.abandon(command_badge, owner);
            return false;
        }

        /* `Request.arguments` is what follows argv[0]: the spawner writes
         * `request.name` as argv[0] itself (specs/environment.md). */
        std::vector<char const *> argument_pointers;
        for (std::string const &arg : args) {
            argument_pointers.push_back(arg.c_str());
        }
        static std::string const kAccountText = "command";
        /* The command's kit, from the one first-class builder (specs/launch.md):
         * the launcher's stream, its runtime, the session's namespace by copy,
         * the doorbell, its own memory copy, its own console (so it opens a
         * window when it wants one), and the clock and timer. */
        aegir::spawn::Child child{};
        child.badge = command_badge;
        child.runtime = command_untyped;
        child.runtime_bits = aegir::terminal::SpawnKit::kCommandUntypedBits;
        child.mem = spawn_kit.command_mem();
        child.stream_badge = kShellStream;
        aegir::spawn::PortGrant ports[8];
        uint32_t const port_count =
            aegir::spawn::command_ports(spawn_kit.kit(), child, ports, 8);
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
        request.badge = command_badge;
        request.give_vspace = true;
        request.untyped_physical = command_untyped_physical;
        request.untyped_bits = aegir::terminal::SpawnKit::kCommandUntypedBits;
        /* The launch request's stack ask (specs/launch.md): zero is the
         * spawner's default, which is what a command gets unless the launcher
         * was asked for more. */
        request.stack_pages = stack_pages;

        aegir::spawn::Process process{};
        if (!spawn_kit.spawner().spawn(request, account, process)) {
            write("  terminal: FAIL spawning a command: ");
            write(spawn_kit.spawner().problem());
            write("\n");
            spawn_kit.abandon(command_badge, owner);
            return false;
        }
        /* The staging is done with: the command is alive, its capabilities are
         * in the pool owned by `owner`, and the window is free for the next
         * command -- which is what lets a background `Run` and the foreground
         * line coexist (specs/memory.md Phase 5). */
        spawn_kit.end_staging();
        live.push_back(LiveCommand{process, command_badge, owner, !background});
        if (!background) {
            ++foreground_outstanding;
        }
        write("  terminal: command started ");
        write(name.c_str());
        write("\n");
        return true;
    };
    /* Reap every foreground command of the line -- the shell has taken its
     * status, or the line failed to start: suspend, release memory, return
     * slots. Background commands are not touched (specs/shell.md's `Run`). */
    auto finish_foreground = [&]() {
        std::vector<LiveCommand> keep;
        for (LiveCommand &record : live) {
            if (record.foreground) {
                spawn_kit.reap(record.process.tcb, record.badge, record.owner);
            } else {
                keep.push_back(record);
            }
        }
        live.swap(keep);
        foreground_outstanding = 0;
        command_stage_count = 0;
        if (live.empty()) {
            spawn_kit.rewind_staging();
        }
    };
    /* A background command has exited: reap it and forget it, without a
     * `return code` line (specs/shell.md). */
    auto reap_background = [&](uint64_t badge) -> bool {
        for (std::size_t i = 0; i < live.size(); ++i) {
            if (live[i].badge == badge && !live[i].foreground) {
                spawn_kit.reap(live[i].process.tcb, live[i].badge, live[i].owner);
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
                if (live.empty()) {
                    spawn_kit.rewind_staging();
                }
                return true;
            }
        }
        return false;
    };
    /* One pipeline stage on the wire: its command line and its own
     * redirections, empty for the console or the connecting pipe. */
    struct Stage {
        std::string line;
        std::string std_in;
        std::string std_out;
    };
    /* Unpack the next string in the call, advancing `at`. */
    auto read_string = [](uint64_t const *words, uint32_t count, uint32_t &at,
                          std::string &out) -> bool {
        char const *text = nullptr;
        uint32_t length = 0;
        if (!aegir::nmspace::unpack_string(words + at, count - at,
                                           aegir::console::kStreamBytesMax, &text,
                                           &length)) {
            return false;
        }
        out.assign(text, length);
        at += packed_words(length);
        return true;
    };
    /* The pipe that connects two stages: named by the terminal, so a pipeline
     * never depends on a name the user chose (specs/pipe.md). */
    auto pipe_path = [](uint64_t serial, uint32_t index) {
        return std::string("PIPE:p") + std::to_string(serial) + "_" +
               std::to_string(index);
    };
    /* Spawn a line's stages, connecting them with pipes the terminal names
     * (specs/pipe.md); `run` is a one-stage pipeline, and a `Run` is one
     * background stage (specs/shell.md). Each command is staged and bracketed
     * on its own, so commands are independent owners. False when a stage would
     * not start (the line's commands are reclaimed). */
    auto spawn_stages = [&](std::vector<Stage> const &stages, std::string const &cwd,
                            std::string const &environment, bool background,
                            uint32_t stack_pages) -> bool {
        /* The environment rides as NUL-separated NAME=VALUE; the spawner wants
         * pointers, so they point into a copy. */
        std::vector<char> environment_buffer(environment.begin(), environment.end());
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
        /* A background `Run` does not take the console: the shell keeps its
         * line editor while the command runs. A command that reads the console
         * is the foreground line's (specs/shell.md). */
        if (!background) {
            command_stage_count = static_cast<uint32_t>(stages.size());
            server.begin_command(kShellStream);
        }
        uint64_t const pipe_serial = ++pipeline_serial;
        uint32_t const stage_count = static_cast<uint32_t>(stages.size());
        bool ok = true;
        for (uint32_t i = 0; ok && i < stage_count; ++i) {
            std::vector<std::string> const words_of_line =
                split_command_words(stages[i].line);
            if (words_of_line.empty()) {
                ok = false;
                break;
            }
            /* The ends keep the stage's own redirection; the middle is the
             * pipe that connects it to its neighbour. */
            std::string const std_in =
                i == 0 ? stages[i].std_in : pipe_path(pipe_serial, i - 1);
            std::string const std_out =
                i + 1 == stage_count ? stages[i].std_out : pipe_path(pipe_serial, i);
            ok = spawn_one(words_of_line[0],
                           std::vector<std::string>(words_of_line.begin() + 1,
                                                    words_of_line.end()),
                           cwd, environment_pointers, std_in, std_out, background,
                           stack_pages);
        }
        if (!ok && !background) {
            finish_foreground();
            server.clear_command(kShellStream);
        }
        return ok;
    };

    /* A untyped of the asked-for size from the memory service, on demand
     * (specs/memory.md): there is no pool to size, and the session's release
     * takes it back. Shared by a nested terminal (its runtime and shell pool)
     * and a windowed program (its runtime). */
    auto alloc_child_mem = [&](uint32_t bits) -> seL4_CPtr {
        aegir::ipc::Consumer const service(spawn_kit.command_mem());
        uint64_t const request = bits;
        uint64_t answer[1] = {};
        bool cap_arrived = false;
        aegir::ipc::WordsReply const reply = service.call_transfer(
            aegir::memory::kMethodAlloc, &request, 1, 0, answer, 1, &cap_arrived);
        if (reply.error != 0 || !cap_arrived) {
            return 0;
        }
        seL4_CPtr const slot = spawn_kit.memory().alloc_slot();
        if (slot == 0 || !aegir::ipc::take_received_cap(slot)) {
            seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                              aegir::bootstrap::kSlotReceiveCap,
                              aegir::bootstrap::cnode_bits());
            return 0;
        }
        return slot;
    };
    /* Reserve the next peer badge out of the range's nested-terminal quarter
     * (specs/launch.md): a nested terminal and a windowed program both draw
     * here, so no two of a session's peers share a serial. False when the
     * quarter is spent. */
    auto reserve_child = [&](uint64_t *base, uint64_t *badge) -> bool {
        if (child_size == 0 || child_count >= 3) {
            return false;
        }
        *base = badge_base + badge_size - (child_count + 1) * child_size;
        ++child_count;
        *badge = aegir::ipc::make_user_badge(aegir::ipc::user_index(own_badge), *base);
        return true;
    };

    /* Launch a nested terminal (specs/launch.md's kind 3): a peer of this
     * terminal, with its own console window, its own shell and its own badge
     * range. The launcher hands it the kit it needs -- the unbadged console,
     * a runtime untyped and a shell pool drawn from mem.main, and the
     * namespace, memory, log, ASID pool, clock and timer to stand up. False
     * when the kit is absent or the range is spent. */
    auto spawn_launched = [&](std::string const &program, std::string const &window,
                              std::vector<std::string> const &arguments) -> bool {
        if (!kit || !spawn_kit.can_launch() || !have_badge_range) {
            write("  terminal: no launcher kit for a nested terminal\n");
            return false;
        }
        uint64_t child_base = 0;
        uint64_t child_badge = 0;
        if (!reserve_child(&child_base, &child_badge)) {
            write("  terminal: the badge range is spent\n");
            return false;
        }
        uint64_t const window_index = child_count - 1;
        constexpr uint32_t kChildUntypedBits = 22;
        if (!load_image("Initrd:" + program)) {
            write("  terminal: no image for the nested terminal\n");
            return false;
        }
        uint32_t const owner = static_cast<uint32_t>(++owner_serial);
        /* The staging and the child's runtime are charged to this terminal's
         * own badge, not the child's: the session's reclaim releases the
         * terminal's badge (specs/auth.md), so a nested terminal's memory comes
         * back at logout even though a nested terminal is never reaped. */
        if (!spawn_kit.begin(owner) || !spawn_kit.begin_command(own_badge)) {
            write("  terminal: FAIL the nested terminal's staging would not begin\n");
            spawn_kit.abandon(own_badge, owner);
            return false;
        }
        seL4_CPtr const child_runtime = alloc_child_mem(kChildUntypedBits);
        seL4_CPtr const child_shell_pool = alloc_child_mem(kChildUntypedBits);
        if (child_runtime == 0 || child_shell_pool == 0) {
            write("  terminal: FAIL no memory for the nested terminal\n");
            spawn_kit.abandon(own_badge, owner);
            return false;
        }
        /* The nested terminal's kit, from the one first-class builder
         * (specs/launch.md): its own console and badge, its runtime, and the
         * unbadged sources it hands its own commands. It is not a launcher of
         * launchers -- depth one -- so it gets no spawn:console.gui yet. */
        aegir::spawn::Child child{};
        child.badge = child_badge;
        child.runtime = child_runtime;
        child.runtime_bits = kChildUntypedBits;
        child.shell_pool = child_shell_pool;
        child.shell_pool_bits = kChildUntypedBits;
        child.launcher = false;
        aegir::spawn::PortGrant ports[12];
        uint32_t const port_count =
            aegir::spawn::launcher_ports(spawn_kit.kit(), child, ports, 12);
        /* The child's environment: this terminal's, less the launcher entries it
         * must not inherit, plus its own range and its window. */
        std::vector<std::string> environment;
        for (char const *const *e = aegir::environment::environ(); *e != nullptr; ++e) {
            std::string const entry(*e);
            if (entry.rfind("AEGIR_BADGE_RANGE=", 0) == 0 ||
                entry.rfind("AEGIR_WINDOW=", 0) == 0 || entry.rfind("AEGIR_FROM=", 0) == 0) {
                continue;
            }
            environment.push_back(entry);
        }
        environment.push_back("AEGIR_BADGE_RANGE=" + std::to_string(child_base + 1) + "," +
                              std::to_string(child_size - 1));
        if (!window.empty()) {
            environment.push_back("AEGIR_WINDOW=" + window);
        } else {
            int const x = kWindowX + 24 * static_cast<int>(window_index);
            int const y = kWindowY + 24 * static_cast<int>(window_index);
            environment.push_back("AEGIR_WINDOW=CON:" + std::to_string(x) + "/" +
                                  std::to_string(y) + "/" + std::to_string(kWindowWidth) +
                                  "/" + std::to_string(kWindowHeight) + "/Terminal");
        }
        std::vector<char const *> environment_pointers;
        environment_pointers.reserve(environment.size());
        for (std::string const &entry : environment) {
            environment_pointers.push_back(entry.c_str());
        }
        /* The words after argv[0] (a `NEWSHELL FROM <file>`): the child
         * terminal's own arguments, which it hands on to the shell it starts
         * (specs/launch.md). */
        std::vector<char const *> argument_pointers;
        argument_pointers.reserve(arguments.size());
        for (std::string const &entry : arguments) {
            argument_pointers.push_back(entry.c_str());
        }
        std::error_code cwd_error;
        std::string const cwd = std::filesystem::current_path(cwd_error).string();
        static char const kName[] = "session.terminal";
        static char const kAccount[] = "terminal";
        aegir::spawn::Request request{};
        request.name = kName;
        request.name_length = sizeof(kName) - 1;
        request.binary_image = image.data();
        request.binary_image_bytes = image.size();
        request.account = kAccount;
        request.account_length = sizeof(kAccount) - 1;
        request.cwd = cwd.c_str();
        request.cwd_length = static_cast<uint32_t>(cwd.size());
        request.environment = environment_pointers.data();
        request.environment_count = static_cast<uint32_t>(environment_pointers.size());
        request.arguments = argument_pointers.empty() ? nullptr : argument_pointers.data();
        request.argument_count = static_cast<uint32_t>(argument_pointers.size());
        request.priority = seL4_MaxPrio - 2;
        request.ports = ports;
        request.port_count = port_count;
        request.fault_endpoint = spawn_kit.fault_endpoint();
        request.badge = child_badge;
        request.give_vspace = true;
        /* A peer is a launcher too, so it gets the larger CSpace (`
         * specs/authority.md`): nesting works at any depth. */
        request.cnode_bits = 13;
        request.untyped_physical = 0;
        request.untyped_bits = kChildUntypedBits;
        aegir::mem::Account account{"terminal", 0, 0, 0};
        aegir::spawn::Process process{};
        if (!spawn_kit.spawner().spawn(request, account, process)) {
            write("  terminal: FAIL spawning a nested terminal: ");
            write(spawn_kit.spawner().problem());
            char const *const detail = spawn_kit.spawner().detail();
            if (detail != nullptr && detail[0] != '\0') {
                write(" (");
                write(detail);
                write(")");
            }
            write("\n");
            spawn_kit.abandon(own_badge, owner);
            return false;
        }
        spawn_kit.end_staging();
        write("  terminal: nested terminal started\n");
        return true;
    };

    /* The shell: its own process, spawned once from its own pool. Its
     * con.stream copy is badged with the shell's stream, and it opens the
     * stream itself -- the terminal is serving before it gets there. It is
     * spawned in on_started, after the ready cue, so the cue is not delayed by
     * the spawn (the acceptance types at it before the demo's zoom). */
    if (kit) {
        app.serve(aegir::ipc::Owner(spawn_kit.kit().stream));
        app.on_call = [&](uint32_t method, uint64_t const *words, uint32_t count,
                          seL4_Word badge, bool cap_arrived, uint64_t *reply,
                          uint32_t capacity) -> uint32_t {
            /* The shell asks the launcher to start a program: the launch
             * request (specs/launch.md). The terminal holds the spawn
             * authority, so it starts the command here, attached to the
             * shell's stream -- the caller's badge. */
            if (method == aegir::console::kStreamMethodBootFail) {
                if (capacity < 1) {
                    return 0;
                }
                /* The boot sequence failed (specs/boot.md): the read-only
                 * failure view. The grid already holds what the sequence
                 * wrote; show the window and take no more input. */
                read_only = true;
                window.show();
                window.request_focus();
                announce_view = true;
                reply[0] = 1;
                return 1;
            }
            if (method == aegir::launch::kMethodSpawn) {
                /* A launch (specs/launch.md): the kind, the flags, the
                 * program's argv, the caller's context (its directory, its
                 * environment and its path), the command's redirected input
                 * and output, a window specification and the stack ask. The
                 * launcher fulfills a command and a launching peer; a kind it
                 * does not know refuses rather than guessing. A command shares
                 * the launcher's console stream, which is the caller's badge,
                 * and carries its own console.gui so it opens a window when it
                 * wants one. */
                if (capacity < 1 || count < 2) {
                    return 0;
                }
                uint32_t at = 0;
                uint64_t const kind = words[at++];
                uint64_t const flags = words[at++];
                Stage stage;
                std::string cwd;
                std::string environment;
                std::string path;
                std::string window;
                if (!read_string(words, count, at, stage.line) ||
                    !read_string(words, count, at, cwd) ||
                    !read_string(words, count, at, environment) ||
                    !read_string(words, count, at, path) ||
                    !read_string(words, count, at, stage.std_in) ||
                    !read_string(words, count, at, stage.std_out) ||
                    !read_string(words, count, at, window) || at >= count) {
                    return 0;
                }
                uint64_t const stack_pages = words[at++];
                (void)path;
                if (kind == aegir::launch::kKindLaunching) {
                    /* A kind-3 peer (specs/launch.md): the launcher resolves the
                     * program from argv[0] and hands it its kit -- its own
                     * console window, a badge range, its own shell. The words
                     * after argv[0] (a `NEWSHELL FROM <file>`) travel as the
                     * child's own arguments. */
                    std::vector<std::string> const words_of_argv =
                        split_command_words(stage.line);
                    std::string const program = words_of_argv.empty()
                                                    ? std::string("aegir-terminal")
                                                    : words_of_argv[0];
                    std::vector<std::string> const arguments(
                        words_of_argv.begin() + (words_of_argv.empty() ? 0 : 1),
                        words_of_argv.end());
                    reply[0] = spawn_launched(program, window, arguments) ? 1 : 0;
                    return 1;
                }
                if (kind != aegir::launch::kKindCommand) {
                    reply[0] = 0;
                    return 1;
                }
                std::vector<Stage> stages;
                stages.push_back(std::move(stage));
                bool const background = (flags & aegir::launch::kFlagBackground) != 0;
                reply[0] = spawn_stages(stages, cwd, environment, background,
                                        static_cast<uint32_t>(stack_pages))
                               ? 1
                               : 0;
                return 1;
            }
            if (method == aegir::launch::kMethodPipeline) {
                /* A pipeline (specs/pipe.md) launched through the same port: a
                 * stage count, each stage's argv and its own redirections,
                 * then the context once. The terminal names the pipes between
                 * the stages. */
                if (capacity < 1 || count < 3) {
                    return 0;
                }
                uint32_t at = 0;
                uint64_t const kind = words[at++];
                uint64_t const flags = words[at++];
                uint32_t const stage_count = static_cast<uint32_t>(words[at++]);
                if (stage_count == 0 || stage_count > count) {
                    reply[0] = 0;
                    return 1;
                }
                std::vector<Stage> stages;
                for (uint32_t i = 0; i < stage_count; ++i) {
                    Stage stage;
                    if (!read_string(words, count, at, stage.line) ||
                        !read_string(words, count, at, stage.std_in) ||
                        !read_string(words, count, at, stage.std_out)) {
                        return 0;
                    }
                    stages.push_back(std::move(stage));
                }
                std::string cwd;
                std::string environment;
                std::string path;
                if (!read_string(words, count, at, cwd) ||
                    !read_string(words, count, at, environment) ||
                    !read_string(words, count, at, path) || at >= count) {
                    return 0;
                }
                uint64_t const stack_pages = words[at++];
                (void)flags;
                (void)path;
                if (kind != aegir::launch::kKindCommand) {
                    reply[0] = 0;
                    return 1;
                }
                reply[0] = spawn_stages(stages, cwd, environment, false,
                                        static_cast<uint32_t>(stack_pages))
                               ? 1
                               : 0;
                return 1;
            }

            if (method == aegir::console::kStreamMethodCommandStatus) {
                /* A pipeline is done only when every stage has reported; until
                 * then there is no status to take and the stream's bracket
                 * must not close (specs/pipe.md). */
                if (foreground_outstanding != 0) {
                    return 0;
                }
                uint32_t const answer =
                    server.handle(method, words, count, badge, reply, capacity);
                if (answer == 1) {
                    /* The shell has taken the status, so the line's commands
                     * are done: stop and reclaim them, so the pool goes back
                     * whole. The status is the last stage's. A pipeline's line
                     * is distinct so a script can wait for the whole pipeline
                     * rather than its first stage (specs/pipe.md). */
                    write("  terminal: command exited ");
                    write_unsigned(reply[0]);
                    write("\n");
                    if (command_stage_count > 1) {
                        write("  terminal: pipeline exited ");
                        write_unsigned(reply[0]);
                        write("\n");
                    }
                    finish_foreground();
                }
                return answer;
            }

            if (method == aegir::console::kStreamMethodExit) {
                /* A command's exit carries its own badge (specs/shell.md), so
                 * the terminal can tell a background `Run`'s exit from the
                 * foreground line's. A background command is reaped here,
                 * without touching the stream's status; a foreground one's
                 * status stays for `command_status`. */
                uint64_t const exit_badge = count >= 2 ? words[1] : 0;
                if (exit_badge != 0 && reap_background(exit_badge)) {
                    write("  terminal: background command exited ");
                    write_unsigned(count >= 1 ? words[0] : 0);
                    write("\n");
                    return 0;
                }
                uint32_t const exit_answer =
                    server.handle(method, words, count, badge, reply, capacity);
                if (foreground_outstanding > 0) {
                    --foreground_outstanding;
                }
                return exit_answer;
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
            if (kit && server.in_command(caller) && spawn_kit.kit().doorbell != 0) {
                seL4_Signal(spawn_kit.kit().doorbell);
            }
        };
    }

    window.set_content(std::move(view));
    window.set_focus(terminal);
    if (spawn_kit.boot_status() == 0) {
        /* The terminal starts focused, so the acceptance's typed line reaches
         * it without a click that would race the demo's. */
        window.request_focus();
        window.show();
    } else {
        /* The boot session's failure view (specs/boot.md): the window is hidden
         * until the boot fails, but its backing is reserved now, because the
         * slice is sized from the windows shown or reserved at start and a
         * window shown later would otherwise have nowhere to draw. */
        window.reserve();
    }
    /* The boot session's window is not shown (specs/boot.md): Startup-Sequence
     * is quiet by default, and its window comes up only when it has output --
     * the failure view's piece (the boot arc's next slice). Its stream and
     * spawner work without a window, so a command the script runs still runs. */

    /* The shell opens its stream a moment after the terminal is up; keys that
     * arrived first are replayed once its editor is there. */
    app.on_poll = [&]() {
        if (announce_view) {
            announce_view = false;
            write("  terminal: boot failed, the view is up\n");
        }
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
        /* The boot session says so, so its ready line is not the session
         * terminal's cue (specs/boot.md). */
        bool const boot = spawn_kit.boot_status() != 0;
        if (nested) {
            /* A launcher started this terminal (specs/launch.md): its own cue,
             * so a nested window is told apart from the session's. */
            write("  terminal: nested ready\n");
        } else {
            write(boot ? "  terminal: boot ready\n" : "  terminal: ready\n");
        }
        if (kit) {
            std::error_code cwd_error;
            std::string const cwd = std::filesystem::current_path(cwd_error).string();
            if (!load_image("Initrd:aegir-shell")) {
                write("  terminal: no image for the shell\n");
            } else {
                /* The boot session's shell is started with Startup-Sequence and
                 * signals auth when it is done; an interactive session's shell
                 * is started with none and runs Shell-Startup itself
                 * (specs/shell.md, specs/boot.md). The boot terminal is the one
                 * auth granted the boot doorbell. */
                static char const kBootScript[] = "Sys:S/Startup-Sequence";
                char const *arguments[1] = {kBootScript};
                uint32_t argument_count = boot ? 1 : 0;
                /* A nested terminal started with `FROM <file>` (specs/launch.md):
                 * its shell runs that file instead of Shell-Startup. */
                if (!boot && !startup_file.empty()) {
                    arguments[0] = startup_file.c_str();
                    argument_count = 1;
                }
                if (!spawn_kit.spawn_shell(image.data(), image.size(), cwd.c_str(),
                                           static_cast<uint32_t>(cwd.size()), kShellStream,
                                           arguments, argument_count)) {
                    write("  terminal: FAIL the shell would not start\n");
                } else {
                    write(boot ? "  terminal: boot shell started\n"
                               : "  terminal: shell started\n");
                }
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