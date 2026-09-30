/*
 * Trinket Requester: a dialog window.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A requester is a window whose content is the caller's and whose bottom
 * button row is the construction's, the message-box shape: the caller builds
 * what goes in the body -- a label, a text field, a file list -- and names the
 * buttons that close it (specs/trinket/overview.md). A MessageBox, a file chooser and
 * an Execute prompt are each a Requester with a different content and set of
 * buttons; the Requester itself is the shared shell.
 *
 * The window's titlebar gadgets are the caller's; the requester adds none. The
 * content is laid out above the buttons at a margin, and the window is sized
 * to fit and centered on the screen. A window must exist before
 * `Application::exec` sizes the console slice it draws into, so a requester is
 * built before the loop starts (its window is reserved, shown later) -- the
 * boot failure view's rule (specs/boot.md).
 */

#ifndef AEGIR_TRINKET_REQUESTER_H
#define AEGIR_TRINKET_REQUESTER_H

#include <aegir/trinket/application.h>
#include <aegir/trinket/point.h>
#include <aegir/trinket/window.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aegir::trinket {

class Panel;
class Widget;

class Requester {
public:
    /** A bottom-row button: the id the caller gets back when it is pressed (or
     *  the window is closed), its label, and its two roles. `is_default` is
     *  the one Enter presses; `is_cancel` is the one Escape and the close
     *  gadget resolve to. */
    struct Action {
        uint32_t id = 0;
        std::u32string label;
        bool is_default = false;
        bool is_cancel = false;
    };

    Requester(Application& app, std::u32string_view title, std::vector<Action> actions);
    Requester(Application& app, std::string_view title, std::vector<Action> actions);
    ~Requester();

    Requester(const Requester&) = delete;
    Requester& operator=(const Requester&) = delete;

    /** The caller's content, above the button row. A zero `size` lets the
     *  content's own preferred size stand. */
    void set_content(std::unique_ptr<Widget> content, Size size = {});

    /** The window's titlebar gadgets (a kGadget* mask). The requester shows
     *  none of its own; a caller that wants a close gadget asks for one. */
    void set_gadgets(uint32_t gadgets);

    /** Size the window to the content and buttons, center it on the screen,
     *  show it, and focus the content (or the default button when there is
     *  none). */
    void show();
    void close();
    bool visible() const;

    Window& window() { return *window_; }
    Application& application() { return app_; }

    /** A bottom button was pressed, or the window was closed: its id. Called
     *  before the window hides, so the caller can still read the content. */
    std::function<void(uint32_t id)> on_action;

private:
    void build(std::u32string title);
    void activate(uint32_t id);
    void layout();

    Application& app_;
    std::unique_ptr<Window> window_;
    /* The window's content root: the caller's widget and the buttons are its
     * children, positioned explicitly (the greeter's shape). */
    Panel* root_ = nullptr;
    Widget* content_ = nullptr;
    Size content_size_{};
    std::vector<Action> actions_;
    std::vector<Widget*> buttons_;
    uint32_t default_id_ = 0;
    uint32_t cancel_id_ = 0;
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_REQUESTER_H
