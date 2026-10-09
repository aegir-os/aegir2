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
#include <aegir/process.h>
#include <aegir/signal.h>
#include <aegir/spawn/process.h>
#include <aegir/spawn/service_kit.h>
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

/* A raw read that is waiting (specs/signal.md): the caller's reply capability
 * is held in a slot of ours until a key arrives or the command ends. One per
 * stream badge -- a caller is single-threaded, so one at a time. */
struct HeldRead {
    aegir::signal::Reply_holder reply;
    uint32_t bound = 0;
    /* Which method is waiting: a raw read (take_read), a line (take_line), or a
     * command's status (take_status). */
    uint32_t method = 0;
};

static std::unordered_map<uint64_t, HeldRead> g_held_reads;
/* A command's status waits in a slot of its own, not the read's (measured: one
 * slot for both is why the shell *polled* -- and a poll here is a blocking call
 * the terminal answers every iteration, so the two of them hold both cores for
 * the whole life of a command and starve the command, the console and the
 * volumes it needs; the floor target's editor steps livelocked exactly that
 * way). Separate slots let a status wait the way a read does, with the
 * command's own held `read` untouched beside it. */
static std::unordered_map<uint64_t, HeldRead> g_held_status;
static bool g_hold_requested = false;
#include <vector>

namespace {

/* The shell's stream is keyed by the badge it was given -- `shell_badge`,
 * reserved below from this terminal's delegated range (specs/process.md) -- so
 * the shell is a process of its own and not a key shared with another. */

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
    aegir::spawn::ServiceKit spawn_kit;
    bool const kit = spawn_kit.adopt(app.allocator(), app.scratch(), app.spawn_slot_base(),
                                    app.spawn_slot_count());
    if (kit) {
        write("  terminal: spawn kit ready\n");
    } else {
        write("  terminal: no spawn kit -- no shell, no commands\n");
    }

    /* The identity and range the session delegated (specs/launch.md): the
     * terminal is a launcher-shaped service, so its shell is a process of the
     * session's class and the shell's own badge comes from that range. */
    uint64_t own_badge = 0;
    if (!aegir::bootstrap::badge(&own_badge)) {
        own_badge = 0;
    }
    spawn_kit.adopt_identity(own_badge);

    /* The shell's own badge (specs/process.md): reserved from that range,
     * before any key can arrive, so the stream server below keys the shell by
     * it. */
    uint64_t const shell_badge = kit ? spawn_kit.reserve_shell_badge() : 0;

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
    window.set_gadgets(kGadgetClose | kGadgetZoom | kGadgetDepth);
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
     * Shell-Startup. `--session` marks the terminal auth started for the
     * session's own composition, whose shell runs `Home:S/User-Startup` once
     * (specs/session.md); a nested terminal does not carry it. */
    std::string startup_file;
    bool session_startup = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        std::string const arg(argv[i]);
        if (arg == "--session") {
            session_startup = true;
        } else if (arg == "FROM" && i + 1 < argc && argv[i + 1] != nullptr) {
            startup_file = argv[i + 1];
            ++i;
        }
    }

    auto view = std::make_unique<TerminalView>();
    /* The terminal is a grid: a fixed advance, so the theme's monospace face
     * (specs/trinket/theming.md), not the proportional default. */
    view->set_font(app.theme().font_monospace());
    view->set_colors(app.theme().color(ColorRole::TEXT),
                     app.theme().color(ColorRole::WINDOW_BG));
    TerminalView *const terminal = view.get();

    aegir::terminal::ConsoleStreamServer server(terminal->buffer());
    /* The grid changed -- the banner, a command's output, the next prompt --
     * so the view that draws it must repaint. */
    server.on_change = [terminal]() { terminal->damage(); };

    /* The line the acceptance's typed steps wait for (specs/console.md): the
     * terminal is worth typing at only when its window has the focus *and* its
     * shell is asking for a line, because the click that gives the focus and
     * the keys ride different queues. This is the greeter's own cue, and the
     * count is what makes each occurrence its own -- a cue that repeats cannot
     * be two steps' (AGENTS.md). Typing at the wrong moment queues the key
     * behind the command that just ended, or drops it on a full event ring. */
    bool input_focused = false;
    bool input_prompted = false;
    bool input_announced = false;
    uint64_t input_lines = 0;
    auto announce_input = [&]() {
        if (!input_focused || !input_prompted || input_announced) {
            return;
        }
        input_announced = true;
        ++input_lines;
        write("  terminal: ready for line ");
        write_unsigned(input_lines);
        /* The stream's own badge, not the window's name: a badge is unique to
         * one stream, while a window's shape is something two terminals can
         * share. The acceptance learns it from the line that already carries it
         * (auth's `session started, badge 0x...`) and each typed step cues on
         * its own (line, shell) pair. */
        write(" shell ");
        write_unsigned(shell_badge);
        write("\n");
    };
    server.on_ready = [&](uint64_t caller) {
        if (caller != shell_badge) {
            return;
        }
        input_prompted = true;
        input_announced = false;
        announce_input();
    };
    /* The focus arrives from the console, the prompt from the shell: whichever
     * is second is the moment both hold, and the line is printed once. */
    window.on_focus_changed = [&](bool focused) {
        input_focused = focused;
        announce_input();
    };

    /* The process registry's caller half (specs/process.md): Ctrl-C sets **C**
     * on the foreground line's pids through it. Auth hands the terminal the
     * port when the session manifest names `process.registry` in its needs, so
     * a session with none just has no interrupt. */
    aegir::ipc::Consumer const registry = aegir::ipc::Consumer::find(
        aegir::process::kPortName, aegir::process::kPortNameLength);

    /* Ctrl-C: the Amiga's Break at the console (specs/process.md). Set **C** on
     * every pid the shell announced for the running line -- a pipeline's every
     * stage, not one -- and hand the shell the break's status, because the
     * enforced halt takes the command back and no stream exit will come. */
    auto break_foreground = [&server, &registry, shell_badge]() {
        std::vector<uint64_t> const pids = server.line_pids(shell_badge);
        for (uint64_t const pid : pids) {
            if (pid == 0) {
                continue;
            }
            uint64_t words[2] = {pid, aegir::process::kAttnC};
            uint64_t reply[1] = {0};
            (void)registry.call_words(aegir::process::kMethodBreak, words, 2, reply, 1);
            write("  terminal: break ");
            write_unsigned(pid);
            write("\n");
        }
        /* The enforced halt took the command back, so a read it had waiting can
         * never be answered -- its reply capability was revoked with the
         * command's TCB. Forget the held read rather than reply into a dead
         * slot (which the kernel logs as a null-cap invocation). */
        g_held_reads.erase(shell_badge);
        server.finish_break(shell_badge);
    };

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
    terminal->on_key = [&server, &pending_keys, &read_only, &registry, &break_foreground,
                        shell_badge](KeyEvent const &event) {
        if (read_only) {
            return false;
        }
        /* Ctrl-C while a command runs (specs/process.md): not a byte on the
         * command's input, but the **C** attention flag on the foreground line.
         * An idle cooked stream, or a session with no registry to break
         * through, keeps the byte for its editor. */
        if (event.pressed && (event.modifiers & kModControl) != 0 &&
            event.text == U'c' && server.in_command(shell_badge) && registry.valid()) {
            break_foreground();
            return true;
        }
        LineEditor *const editor = server.editor(shell_badge);
        bool const ready =
            editor != nullptr && (editor->editing() || server.in_command(shell_badge));
        /* Hold the key when the shell has not begun its prompt yet (or is
         * between commands), and also when earlier keys are still held: a key
         * fed straight to the editor would otherwise overtake one on_poll has
         * not replayed, and the line would read out of order. on_poll replays
         * the held keys, in order, once the editor is ready. */
        if (!ready || !pending_keys.empty()) {
            pending_keys.push_back(event);
            if (server.on_wake) {
                server.on_wake(shell_badge);
            }
            return true;
        }
        return server.on_key(shell_badge, event);
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


    /* The shell: its own process, spawned once from its own pool. Its
     * con.stream copy is badged with the shell's stream, and it opens the
     * stream itself -- the terminal is serving before it gets there. It is
     * spawned in on_started, after the ready cue, so the cue is not delayed by
     * the spawn (the acceptance types at it before the demo's zoom). */
    if (kit) {
        app.serve(aegir::ipc::Owner(spawn_kit.kit().stream));
        /* The session's launcher (specs/launch.md): the shell launches through
         * it now, and a command's exit is reported on this stream, so the
         * terminal asks the launcher to take the finished command back --
         * suspend it, release its memory, return its slots (specs/memory.md).
         * The terminal learns the stream's state from the stream, not from who
         * spawned whom. */
        aegir::ipc::Consumer const launcher = aegir::ipc::Consumer::find(
            aegir::launch::kPortName, aegir::launch::kPortNameLength);
        auto release_command = [&](uint64_t command_badge) -> uint64_t {
            if (!launcher.valid() || command_badge == 0) {
                return 0;
            }
            uint64_t words[1] = {command_badge};
            uint64_t answer[1] = {};
            aegir::ipc::WordsReply const reply = launcher.call_words(
                aegir::launch::kMethodRelease, words, 1, answer, 1);
            return reply.error == 0 && reply.count >= 1 ? answer[0] : 0;
        };
        /* The exit cue, in one place: the immediate answer and the held one
         * (on_wake) both owe it, and the runner's steps are cued on it. Printed
         * before the status is released to the shell, so nothing the shell
         * writes once it has the status can overtake the cue. */
        auto report_status = [&](uint64_t caller, uint64_t status) {
            write("  terminal: command exited ");
            write_unsigned(status);
            write("\n");
            if (server.stages(caller) > 1) {
                write("  terminal: pipeline exited ");
                write_unsigned(status);
                write("\n");
            }
        };
        app.on_call = [&](uint32_t method, uint64_t const *words, uint32_t count,
                          seL4_Word badge, bool cap_arrived, uint64_t *reply,
                          uint32_t capacity) -> uint32_t {
            /* The terminal serves the stream now, not the spawn: the shell
             * launches through the session's launcher (specs/launch.md), and
             * what the terminal still owns is the stream -- the line editor,
             * the input queue, and the finished command's report, which
             * arrives here because the command's stream is this one. */
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
            if (method == aegir::console::kStreamMethodCommandStatus) {
                /* The shell reads the line's status, and it is due only when
                 * every stage has reported (specs/pipe.md, specs/signal.md).
                 * While the line still runs there is nothing to take, and the
                 * reply is *held*, not refused: a caller that is refused comes
                 * straight back for it, and that is a poll -- a blocking call
                 * this terminal must answer every iteration. The shell and the
                 * terminal then hold both cores for the whole life of the
                 * command, and the command, the console and the volumes it
                 * needs are starved; measured on the floor target, that is a
                 * livelock at the editor's steps (the shell's poll seen at 200k
                 * iterations with the serial gone silent behind it). The status
                 * waits in a slot of its own (g_held_status) so the command's
                 * held `read` is untouched, and it is answered from on_wake when
                 * the line completes -- by its exit, or by a break whose
                 * finish_break completes the line when no exit is coming. */
                if (server.in_command(badge) && !server.line_complete(badge)) {
                    /* Cleared before the ask, the way every path that may hold
                     * does: a stale set from an earlier hold would otherwise
                     * answer kHoldReply for a hold that was not taken, and the
                     * caller's reply would then never be sent at all. */
                    g_hold_requested = false;
                    server.on_hold(badge, aegir::console::kStreamMethodCommandStatus, 0);
                    return g_hold_requested ? app.kHoldReply : 0;
                }
                g_hold_requested = false;
                uint32_t const answer =
                    server.handle(method, words, count, badge, reply, capacity);
                if (g_hold_requested) {
                    return app.kHoldReply;
                }
                if (answer == 1) {
                    report_status(badge, reply[0]);
                }
                return answer;
            }

            if (method == aegir::console::kStreamMethodExit) {
                /* The launcher is the one that knows the command: its answer to
                 * the release says whether the command held the caller's line
                 * or was a background `Run`, and 0 for a badge it does not hold
                 * -- a *shell* closing its own con.stream, whose exit report
                 * rides the same path (the runtime sends one when the process
                 * ends, specs/shell.md). A shell's exit is nothing to report,
                 * and a background exit leaves the stream's status untouched
                 * (specs/signal.md, specs/terminal.md). */
                uint64_t const exit_badge = count >= 2 ? words[1] : 0;
                uint64_t const released = release_command(exit_badge);
                if (released == 0) {
                    return 0;
                }
                if (released == 2) {
                    write("  terminal: background command exited ");
                    write_unsigned(count >= 1 ? words[0] : 0);
                    write("\n");
                    return 0;
                }
                return server.handle(method, words, count, badge, reply, capacity);
            }

            g_hold_requested = false;
            uint32_t const answer =
                server.handle(method, words, count, badge, reply, capacity);
            if (g_hold_requested) {
                /* The read found nothing queued: its reply is held and the
                 * handler saved the caller's reply capability, so the toolkit
                 * must not answer for it (specs/signal.md). */
                return app.kHoldReply;
            }
            if (cap_arrived) {
                /* No client passes a capability at open any more -- a read
                 * waits by holding its reply (specs/signal.md) -- but one that
                 * does is taken out of the scratch slot and dropped, so the
                 * next receive finds it empty. */
                seL4_CPtr const slot = app.alloc_slot();
                if (slot != 0 && aegir::ipc::take_received_cap(slot)) {
                    seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, slot,
                                      aegir::bootstrap::endpoint_depth());
                }
            }
            return answer;
        };
        /* The handler says the stream has something to read; the terminal
         * answers the read that was waiting for it (specs/signal.md). The
         * handler is a pure value and knows no kernel, so the answer lives
         * here. */
        server.on_wake = [&](uint64_t caller) {
            /* A raw read that was waiting is answered here: the bytes when
             * there are any, end of input when the command has ended, and
             * nothing when there is still nothing (specs/signal.md). */
            auto held = g_held_reads.find(caller);
            if (held != g_held_reads.end()) {
                uint64_t reply[aegir::ipc::kMaxWords];
                uint32_t const n =
                    held->second.method == aegir::console::kStreamMethodRead
                        ? server.take_read(caller, held->second.bound, reply,
                                           aegir::ipc::kMaxWords)
                        : server.take_line(caller, reply, aegir::ipc::kMaxWords);
                if (n != 0) {
                    held->second.reply.reply(reply, n, 0);
                    g_held_reads.erase(held);
                } else if (held->second.method == aegir::console::kStreamMethodRead &&
                           server.command_finished(caller)) {
                    held->second.reply.reply(nullptr, 0, 0);
                    g_held_reads.erase(held);
                }
            }
            /* And a command's status: the line has completed, so the wait the
             * shell is sitting in ends here. This is the ordinary way a held
             * status is answered -- the immediate path answers only the case
             * where the line was already done when the shell asked -- and the
             * cue goes out before the reply, as it does there. */
            auto due = g_held_status.find(caller);
            if (due != g_held_status.end()) {
                uint64_t reply[aegir::ipc::kMaxWords];
                uint32_t const n = server.take_status(caller, reply, aegir::ipc::kMaxWords);
                if (n != 0) {
                    report_status(caller, reply[0]);
                    due->second.reply.reply(reply, n, 0);
                    g_held_status.erase(due);
                }
            }
        };
        /* A read that found nothing waits: save the caller's reply capability
         * and answer it from on_wake when a key or the command's end arrives
         * (specs/signal.md). A command's status waits the same way, in a slot of
         * its own, so a command holding a read does not take the status's. */
        server.on_hold = [&](uint64_t caller, uint32_t method, uint32_t bound) {
            auto &table = method == aegir::console::kStreamMethodCommandStatus
                              ? g_held_status
                              : g_held_reads;
            /* One of each kind per caller: a caller is single-threaded, so a
             * second of the same kind means the first was never answered, and
             * replacing it would drop that reply for ever. The new call is left
             * to be answered as "not yet" instead, which leaves its caller to
             * ask again. */
            if (table.find(caller) != table.end()) {
                return;
            }
            seL4_CPtr const slot = app.alloc_slot();
            if (slot == 0) {
                return;
            }
            HeldRead held;
            held.reply = aegir::signal::Reply_holder(aegir::bootstrap::kSlotOwnCNode,
                                                     aegir::bootstrap::endpoint_depth(),
                                                     slot);
            held.bound = bound;
            held.method = method;
            if (!held.reply.save()) {
                return;
            }
            table[caller] = held;
            g_hold_requested = true;
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
        /* Replay the keys held while the shell was between prompts, one line's
         * worth at a time (specs/terminal.md). A key that ends a line stops the
         * editor, and the keys behind it belong to the next prompt: feeding
         * them now would lose them, because the editor ignores a key while it
         * is not editing, and the shell would wait at a prompt no line ever
         * reaches. So the replay stops at the end of the line it fed and
         * resumes on the next poll, once the shell has begun the next prompt. */
        while (!pending_keys.empty()) {
            LineEditor *const editor = server.editor(shell_badge);
            bool const command = server.in_command(shell_badge);
            if (editor == nullptr || (!editor->editing() && !command)) {
                return;
            }
            KeyEvent const event = pending_keys.front();
            pending_keys.erase(pending_keys.begin());
            (void)server.on_key(shell_badge, event);
            if (!command && !editor->editing()) {
                return;
            }
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
                 * auth granted the boot status. */
                static char const kBootScript[] = "Sys:S/Startup-Sequence";
                static char const kSessionStartup[] = "--session";
                char const *arguments[1] = {kBootScript};
                uint32_t argument_count = boot ? 1 : 0;
                /* A nested terminal started with `FROM <file>` (specs/launch.md):
                 * its shell runs that file instead of Shell-Startup. */
                if (!boot && !startup_file.empty()) {
                    arguments[0] = startup_file.c_str();
                    argument_count = 1;
                } else if (!boot && session_startup) {
                    /* The session's own terminal (specs/session.md): its shell
                     * runs `Home:S/User-Startup` once, before Shell-Startup. */
                    arguments[0] = kSessionStartup;
                    argument_count = 1;
                }
                if (!spawn_kit.spawn_shell(image.data(), image.size(), cwd.c_str(),
                                           static_cast<uint32_t>(cwd.size()),
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