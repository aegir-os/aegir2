/*
 * aegir-output: the launcher's read-only output view (specs/launch.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A caller with no console stream of its own -- a launching program, a greeter,
 * the Bureau -- starts a command through launch.session, and the launcher needs
 * a stream to give that command. This is it: started by the launcher, it owns
 * the con.stream endpoint the command writes to and draws what arrives in a
 * window. It takes no input (a read answers empty), so it is a view, not a
 * terminal.
 *
 * It is also a launcher client: the command's exit carries the command's own
 * badge, and the view releases it through launch.session, because no shell
 * holds the command's line to reap it (specs/memory.md Phase 5). One view per
 * launcher serves every stream-less command.
 */

#include <aegir/bootstrap.h>
#include <aegir/console.h>
#include <aegir/console_stream.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/launch.h>
#include <aegir/log.h>
#include <aegir/nmspace.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/terminal_view.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/window.h>
#include <sel4/sel4.h>
#include <cstdint>
#include <memory>
#include <string_view>

namespace {

void write(char const *text)
{
    aegir::debug_write(text);
}

/* Clear of the terminal's window, the demo's and the viewer's. */
constexpr int kWindowX = 40;
constexpr int kWindowY = 420;
constexpr int kWindowWidth = 560;
constexpr int kWindowHeight = 300;

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

    aegir::ipc::Consumer const gui = aegir::ipc::Consumer::find(
        aegir::console::kPortName, aegir::console::kPortNameLength);
    if (!gui.valid()) {
        write("  output: FAIL no console.gui\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    app.set_gui_port(gui);

    /* The stream the launcher made for us and the command writes to: we are
     * its owner, so we serve it (the endpoint, not a caller half). */
    aegir::ipc::Consumer const stream = aegir::ipc::Consumer::find(
        aegir::console::kStreamPortName, aegir::console::kStreamPortNameLength);
    if (!stream.valid()) {
        write("  output: FAIL no con.stream to own\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    app.serve(aegir::ipc::Owner(stream.capability()));

    /* Release a finished command through the launcher (specs/launch.md): the
     * exit report carries the command's own badge. */
    aegir::ipc::Consumer const launcher =
        aegir::ipc::Consumer::find(aegir::launch::kPortName, aegir::launch::kPortNameLength);

    Window window(app);
    window.set_title("Output");
    window.set_rect({kWindowX, kWindowY, kWindowWidth, kWindowHeight});
    window.set_gadgets(kGadgetClose | kGadgetZoom | kGadgetDepth);
    window.on_close_requested = [&app]() { app.quit(0); };

    auto view = std::make_unique<TerminalView>();
    view->set_font(app.default_font());
    view->set_colors(app.theme().color(ColorRole::TEXT),
                     app.theme().color(ColorRole::WINDOW_BG));
    view->set_cursor_visible(false);
    TerminalView* const view_ptr = view.get();

    app.on_call = [&](uint32_t method, uint64_t const *words, uint32_t count,
                      seL4_Word badge, bool cap_arrived, uint64_t *reply,
                      uint32_t capacity) -> uint32_t {
        static_cast<void>(badge);
        static_cast<void>(cap_arrived);
        static_cast<void>(capacity);
        if (method == aegir::console::kStreamMethodOpen) {
            reply[0] = 1;
            return 1;
        }
        if (method == aegir::console::kStreamMethodWrite) {
            char const *text = nullptr;
            uint32_t length = 0;
            if (aegir::nmspace::unpack_string(words, count, aegir::console::kStreamBytesMax,
                                              &text, &length)) {
                view_ptr->buffer().write(std::string_view(text, length));
                view_ptr->buffer().scroll_to_bottom();
                view_ptr->damage();
            }
            reply[0] = length;
            return 1;
        }
        if (method == aegir::console::kStreamMethodSize) {
            reply[0] = static_cast<uint64_t>(view_ptr->buffer().rows());
            reply[1] = static_cast<uint64_t>(view_ptr->buffer().columns());
            return 2;
        }
        if (method == aegir::console::kStreamMethodExit) {
            /* status, then the command's own badge. Release it so its memory
             * and pool slots return (specs/memory.md Phase 5). */
            uint64_t const command_badge = count >= 2 ? words[1] : 0;
            if (launcher.valid() && command_badge != 0) {
                uint64_t answer[1] = {};
                (void)launcher.call_words(aegir::launch::kMethodRelease, &command_badge, 1,
                                          answer, 1);
            }
            write("  output: command done\n");
            return 0;
        }
        /* A read is a view's empty answer: this stream takes no input. open's
         * mode, close, set_prompt, command_status, line and boot_fail are
         * nothing to a view. */
        return 0;
    };

    window.set_content(std::move(view));
    window.show();

    app.on_started = [&]() {
        write("  output: ready\n");
        if (log.valid()) {
            (void)log.call(aegir::log::kMethodEvent,
                           static_cast<uint64_t>(aegir::log::Event::Ready));
        }
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
    };

    return app.exec();
}
