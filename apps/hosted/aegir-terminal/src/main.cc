/*
 * aegir-terminal: the session's CON: handler and command line.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The terminal owns one console window and a text grid, serves con.stream on
 * its own endpoint, and runs the shell as its first client. Auth starts it
 * beside the bureau at login with the console, the namespace, its Home, and
 * the spawn kit that lets it run the session's commands (specs/terminal.md,
 * specs/shell.md, specs/authority.md).
 *
 * A command is spawned with a caller copy of the shell's stream, so its
 * output lands on the same grid the shell writes to, and reports the status
 * it finished with (the interim until exit() carries one, Phase 4). The shell
 * is in-process for now; the stream it uses is the same one a command reaches
 * over the port.
 */

#include "shell.h"
#include "spawn_kit.h"

#include <aegir/bootstrap.h>
#include <aegir/console.h>
#include <aegir/console_stream.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/spawn/process.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/line_editor.h>
#include <aegir/trinket/terminal_view.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/unicode.h>
#include <aegir/trinket/window.h>
#include <sel4/sel4.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

/* The shell's stream, keyed by this number: a command's con.stream cap is
 * badged with it, so the command writes to the shell's stream rather than
 * opening one of its own (specs/terminal.md). Not a badge the kernel minted,
 * just a key inside the handler. */
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

    /* The authority to start the session's commands (specs/authority.md,
     * specs/shell.md): auth delegates it, the terminal adopts it into the
     * toolkit's one allocator. A failure here turns external commands off and
     * leaves the built-in command line working. */
    aegir::terminal::SpawnKit spawn_kit;
    if (spawn_kit.adopt(app)) {
        write("  terminal: spawn kit ready\n");
    } else {
        write("  terminal: no spawn kit -- external commands are off\n");
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
    TerminalView* const terminal = view.get();

    aegir::terminal::ConsoleStreamServer server(terminal->buffer());
    auto shell = std::make_unique<aegir::terminal::Shell>(
        server, kShellStream, [&app]() { app.quit(0); });

    /* The terminal's own editor for the shell's stream: the server made it, and
     * the view feeds it keys. A key while a command runs finds no editor (the
     * line was finished) and is ignored. */
    terminal->on_key = [&server](KeyEvent const &event) {
        LineEditor *const editor = server.editor(kShellStream);
        return editor != nullptr && editor->on_key(event);
    };

    uint64_t command_serial = 0;
    auto spawn_command = [&](std::string const &name,
                             std::vector<std::string> const &args) -> bool {
        if (!spawn_kit.ready()) {
            return false;
        }
        /* The image comes from Initrd: through the namespace, not from a
         * mapped initrd: it is 5.7 MiB and does not fit a child
         * (specs/shell.md, specs/authority.md). */
        std::string const path = "Initrd:" + name;
        std::FILE *const file = std::fopen(path.c_str(), "rb");
        if (file == nullptr) {
            write("  terminal: no image for the command\n");
            return false;
        }
        std::vector<char> image;
        char chunk[512];
        std::size_t have = 0;
        while ((have = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
            image.insert(image.end(), chunk, chunk + have);
        }
        std::fclose(file);
        if (image.empty()) {
            write("  terminal: the command image is empty\n");
            return false;
        }

        /* `Request.arguments` is what follows argv[0]: the spawner writes
         * `request.name` as argv[0] itself (specs/environment.md). */
        std::vector<char const *> argument_pointers;
        for (std::string const &arg : args) {
            argument_pointers.push_back(arg.c_str());
        }
        static std::string const kAccountText = "command";
        std::string const cwd = shell->current_directory();
        aegir::mem::Account account{"command", 0, 0, 0};
        aegir::spawn::PortGrant const ports[] = {
            {aegir::console::kStreamPortName, aegir::console::kStreamPortNameLength,
             aegir::bootstrap::kSlotFirstDeclared, spawn_kit.stream_endpoint(),
             seL4_CapRights_new(1, 1, 0, 1), kShellStream, 0},
        };
        aegir::spawn::Request request{};
        request.name = name.c_str();
        request.name_length = static_cast<uint32_t>(name.size());
        request.binary_image = image.data();
        request.binary_image_bytes = image.size();
        request.account = kAccountText.c_str();
        request.account_length = static_cast<uint32_t>(kAccountText.size());
        request.arguments = argument_pointers.data();
        request.argument_count = static_cast<uint32_t>(args.size());
        request.cwd = cwd.c_str();
        request.cwd_length = static_cast<uint32_t>(cwd.size());
        request.priority = seL4_MaxPrio - 2;
        request.ports = ports;
        request.port_count = 1;
        request.fault_endpoint = spawn_kit.fault_endpoint();
        request.badge = 0x1000 + command_serial++;
        request.give_vspace = false;

        aegir::spawn::Process process{};
        if (!spawn_kit.spawner().spawn(request, account, process)) {
            write("  terminal: FAIL spawning a command: ");
            write(spawn_kit.spawner().problem());
            write("\n");
            return false;
        }
        return true;
    };
    shell->set_spawn(spawn_command);

    /* The shell is the stream's first client, and the terminal serves the port
     * a command reaches it through. Only the port needs the kit. */
    if (spawn_kit.ready()) {
        app.serve(aegir::ipc::Owner(spawn_kit.stream_endpoint()));
        app.on_call = [&server](uint32_t method, uint64_t const *words, uint32_t count,
                                seL4_Word badge, bool, uint64_t *reply,
                                uint32_t capacity) {
            return server.handle(method, words, count, badge, reply, capacity);
        };
    }

    window.set_content(std::move(view));
    window.set_focus(terminal);
    /* The terminal starts focused, so the acceptance's typed line reaches it
     * without a click that would race the demo's. */
    window.request_focus();
    window.show();

    /* A finished line runs the shell; a command's exit draws the next prompt.
     * Both happen here rather than in the key callback, because a command's
     * output arrives between the Enter and the prompt (specs/shell.md). */
    app.on_poll = [&]() {
        if (server.line_ready(kShellStream)) {
            std::u32string const line = server.take_line(kShellStream);
            write("  terminal: line ");
            write(aegir::trinket::utf32_to_utf8(line).c_str());
            write("\n");
            shell->run_line(line);
        }
        if (shell->busy() && server.command_finished(kShellStream)) {
            uint64_t const status = server.exit_status(kShellStream);
            server.clear_command(kShellStream);
            write("  terminal: command exited ");
            write_unsigned(status);
            write("\n");
            shell->command_finished(status);
        }
    };

    app.on_started = [&]() {
        write("  terminal: ready\n");
        shell->start();
        if (log.valid()) {
            (void)log.call(aegir::log::kMethodEvent,
                           static_cast<uint64_t>(aegir::log::Event::Ready));
        }
        /* No supervision signal: this session is long-lived, like the bureau
         * (specs/workbench.md). */
    };

    return app.exec();
}
