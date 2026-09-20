/*
 * aegir-greeter: the login window.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Auth starts one of these once the user database is read (specs/console.md's
 * login arc): auth's face, a system child badged from auth's own range
 * (specs/authority.md), asking on the screen what the serial line asked
 * before it. It is the toolkit's first client (specs/trinket.md): a trinket
 * Application adopts its spawn kit and the console's channel, a Window owns
 * the console window, and a Panel of Labels, TextBoxes and a Button is the
 * form. The name field takes the focus; Tab moves to the secret, which echoes
 * bullets; Enter or the button asks auth.login. A refused answer clears the
 * secret and shows the error line. An accepted one is this process's exit:
 * auth started the session the login earned, and the window and slice go back
 * with auth's reap of this badge -- the greeter draws nothing after the
 * reply, so the teardown's revoke never meets a live mapping in here.
 */

#include <aegir/authdb.h>
#include <aegir/bootstrap.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/nmspace.h>
#include <aegir/trinket/application.h>
#include <aegir/trinket/button.h>
#include <aegir/trinket/label.h>
#include <aegir/trinket/panel.h>
#include <aegir/trinket/textbox.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/window.h>
#include <sel4/sel4.h>
#include <memory>
#include <string>

namespace {

void write(char const *text)
{
    aegir::debug_write(text);
}

void write(char const *text, uint32_t length)
{
    aegir::debug_write(text, length);
}

/* The form's geometry, window-local. The fields sit under their labels, the
 * button below, the refusal line at the bottom. The form is laid out
 * explicitly rather than by a layout: it is one form, and the toolkit's
 * layouts are for the clients that need them. */
constexpr int kWindowX = 400;
constexpr int kWindowY = 220;
constexpr int kWindowWidth = 480;
constexpr int kWindowHeight = 360;

constexpr int kFieldX = 24;
constexpr int kFieldWidth = 432;
constexpr int kFieldHeight = 24;
constexpr int kNameLabelY = 40;
constexpr int kNameFieldY = 56;
constexpr int kSecretLabelY = 104;
constexpr int kSecretFieldY = 120;
constexpr int kButtonX = 24;
constexpr int kButtonY = 168;
constexpr int kButtonWidth = 120;
constexpr int kButtonHeight = 32;
constexpr int kErrorY = 224;

/* The login ask (aegir/authdb.h's wire): the name's words, then the secret's,
 * one word back -- 1 authenticated, 0 refused. */
uint64_t login(aegir::ipc::Consumer const &port, std::string const &name,
               std::string const &secret) noexcept
{
    uint64_t out[aegir::ipc::kMaxWords];
    uint32_t words = aegir::nmspace::pack_string(
        out, name.data(), static_cast<uint32_t>(name.size()),
        aegir::authdb::kNameBytes);
    if (words == 0) {
        return ~0ULL;
    }
    uint32_t const secret_words = aegir::nmspace::pack_string(
        out + words, secret.data(), static_cast<uint32_t>(secret.size()),
        aegir::authdb::kSecretBytes);
    if (secret_words == 0) {
        return ~0ULL;
    }
    words += secret_words;
    uint64_t in[1];
    aegir::ipc::WordsReply const answer =
        port.call_words(aegir::auth::kMethodLogin, out, words, in, 1);
    if (answer.error != 0 || answer.count != 1) {
        return ~0ULL;
    }
    return in[0];
}

}  // namespace

int main(int argc, char *argv[])
{
    using namespace aegir::trinket;

    Application &app = Application::create(argc, argv);

    /* The ports the spawn kit installed: the console's window protocol and
     * auth's login, both badged with this process's badge by auth. */
    aegir::ipc::Consumer const gui = aegir::ipc::Consumer::find(
        aegir::console::kPortName, aegir::console::kPortNameLength);
    aegir::ipc::Consumer const auth_login =
        aegir::ipc::Consumer::find(aegir::auth::kPortName, aegir::auth::kPortNameLength);
    if (!gui.valid() || !auth_login.valid()) {
        write("  greeter: FAIL no console.gui or auth.login\n");
        aegir::halt();
    }
    app.set_gui_port(gui);

    Theme &theme = app.theme();

    Window window(app);
    window.set_title("Aegir");
    window.set_rect({kWindowX, kWindowY, kWindowWidth, kWindowHeight});

    auto panel = std::make_unique<Panel>(Panel::Style::FLAT);
    panel->set_background(theme.color(ColorRole::WINDOW_BG));
    panel->set_layout(nullptr);

    auto name_label = std::make_unique<Label>("Username:");
    name_label->set_text_color(theme.color(ColorRole::TEXT));
    name_label->set_rect({kFieldX, kNameLabelY, 60, 12});
    panel->add_child(std::move(name_label));

    auto name_box = std::make_unique<TextBox>();
    name_box->set_rect({kFieldX, kNameFieldY, kFieldWidth, kFieldHeight});
    name_box->set_max_length(aegir::authdb::kNameBytes - 1);
    TextBox *const name = name_box.get();
    panel->add_child(std::move(name_box));

    auto secret_label = std::make_unique<Label>("Password:");
    secret_label->set_text_color(theme.color(ColorRole::TEXT));
    secret_label->set_rect({kFieldX, kSecretLabelY, 60, 12});
    panel->add_child(std::move(secret_label));

    auto secret_box = std::make_unique<TextBox>();
    secret_box->set_rect({kFieldX, kSecretFieldY, kFieldWidth, kFieldHeight});
    secret_box->set_max_length(aegir::authdb::kSecretBytes - 1);
    secret_box->set_password_mode(true);
    TextBox *const secret = secret_box.get();
    panel->add_child(std::move(secret_box));

    auto error_label = std::make_unique<Label>("no such name or secret");
    error_label->set_text_color(theme.color(ColorRole::ERROR));
    error_label->set_rect({kFieldX, kErrorY, 200, 12});
    error_label->set_visible(false);
    Label *const error = error_label.get();
    panel->add_child(std::move(error_label));

    auto submit_button = std::make_unique<Button>("Login");
    submit_button->set_rect({kButtonX, kButtonY, kButtonWidth, kButtonHeight});
    Button *const button = submit_button.get();
    panel->add_child(std::move(submit_button));

    window.set_content(std::move(panel));
    /* The name field takes the first keystroke, as the serial prompt took the
     * name before it. */
    window.set_focus(name);
    window.show();

    auto attempt_login = [&]() {
        uint64_t const answer = login(auth_login, name->text_utf8(), secret->text_utf8());
        if (answer == 1) {
            std::string const who = name->text_utf8();
            write("  greeter: welcome, ");
            write(who.data(), static_cast<uint32_t>(who.size()));
            write(" -- the bureau takes it from here\n");
            /* This process is done; auth reaps the window and slice with this
             * badge, so nothing below touches them. */
            seL4_Signal(aegir::bootstrap::kSlotSupervision);
            aegir::halt();
        }
        /* Refused -- or the ask itself broke, which reads the same to the
         * person at the screen. The secret does not survive a refusal. */
        error->set_visible(true);
        secret->set_text(std::string_view{});
    };
    button->on_click = [&](bool) { attempt_login(); };
    name->on_submit = [&]() { attempt_login(); };
    secret->on_submit = [&]() { attempt_login(); };

    bool focus_announced = false;
    window.on_focus_changed = [&](bool focused) {
        /* The focus is announced once, and it is the runner's cue to type
         * (scripts/targets.py). */
        if (focused && !focus_announced) {
            focus_announced = true;
            write("  greeter: the window has the focus\n");
        }
    };

    /* The supervision signal is the spawner's clock: this one says the form
     * is on the screen (auth waits for it before the boot moves on). */
    app.on_started = [&]() {
        write("  greeter: a name and a secret, please\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
    };

    return app.exec();
}
