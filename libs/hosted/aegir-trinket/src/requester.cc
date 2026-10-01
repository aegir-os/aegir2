/*
 * Trinket Requester implementation (see the header for the shape).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/requester.h>

#include <aegir/trinket/button.h>
#include <aegir/trinket/panel.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/unicode.h>

#include <algorithm>

namespace aegir::trinket {

namespace {

/* The dialog's spacing, in logical pixels. Explicit, like the greeter's form:
 * the toolkit's layouts are for the clients that arrange. */
constexpr int kMargin = 12;
constexpr int kGap = 10;      /* content to the button row */
constexpr int kButtonGap = 8; /* button to button */

}  // namespace

Requester::Requester(Application& app, std::u32string_view title,
                     std::vector<Action> actions)
    : app_(app), actions_(std::move(actions))
{
    build(std::u32string(title));
}

Requester::Requester(Application& app, std::string_view title,
                     std::vector<Action> actions)
    : app_(app), actions_(std::move(actions))
{
    build(utf8_to_utf32(title));
}

Requester::~Requester() = default;

void Requester::build(std::u32string title)
{
    window_ = std::make_unique<Window>(app_);
    window_->set_title(title);
    window_->set_resizable(false);
    /* Depth alone: the requester adds no close, and a caller that wants one
     * (and the cancel it resolves to) asks with set_gadgets. */
    window_->set_gadgets(kGadgetDepth);
    /* Reserve the backing now: the console gives one slice per process, sized
     * at exec, so a window shown later must say it is coming. */
    window_->reserve();

    auto root = std::make_unique<Panel>(Panel::Style::FLAT);
    root->set_background(app_.theme().color(ColorRole::WINDOW_BG));
    root->set_layout(nullptr);
    root_ = root.get();

    for (Action const& action : actions_) {
        auto button = std::make_unique<Button>(action.label);
        Button* const raw = button.get();
        uint32_t const id = action.id;
        button->on_click = [this, id](bool) { activate(id); };
        root->add_child(std::move(button));
        buttons_.push_back(raw);
        if (action.is_default) {
            default_id_ = id;
        }
        if (action.is_cancel) {
            cancel_id_ = id;
        }
    }

    window_->set_content(std::move(root));
    window_->on_close_requested = [this]() {
        if (cancel_id_ != 0) {
            activate(cancel_id_);
        } else {
            close();
        }
    };
    window_->on_key = [this](KeyEvent const& key) -> bool {
        if (!key.pressed) {
            return false;
        }
        if (key.code == KeyCode::ESCAPE && cancel_id_ != 0) {
            activate(cancel_id_);
            return true;
        }
        if (key.code == KeyCode::ENTER && default_id_ != 0) {
            activate(default_id_);
            return true;
        }
        return false;
    };
}

void Requester::set_content(std::unique_ptr<Widget> content, Size size)
{
    content_ = content.get();
    content_size_ = size;
    root_->add_child(std::move(content));
    /* Size the window now, so the slice the console gives is sized for the
     * content the window will draw (the header's build-before-exec rule). */
    layout();
}

void Requester::set_gadgets(uint32_t gadgets)
{
    window_->set_gadgets(gadgets);
}

void Requester::set_stays_open(uint32_t id)
{
    stays_open_.push_back(id);
}

void Requester::show()
{
    layout();
    Widget* const focus = content_ != nullptr && content_->focusable()
                              ? content_
                              : (buttons_.empty() ? nullptr : buttons_.front());
    if (focus != nullptr) {
        window_->set_focus(focus);
    }
    window_->show();
    window_->request_focus();
}

void Requester::close()
{
    window_->hide();
}

bool Requester::visible() const
{
    return window_->visible();
}

void Requester::activate(uint32_t id)
{
    /* The caller reads its content here, before the window hides and the tree
     * is no longer on screen. */
    if (on_action) {
        on_action(id);
    }
    /* An action that acts on the open dialog -- Volumes, Parent -- leaves it up;
     * the default and the cancel close it. */
    for (uint32_t const stays : stays_open_) {
        if (stays == id) {
            return;
        }
    }
    close();
}

void Requester::layout()
{
    Size content{0, 0};
    if (content_ != nullptr) {
        content = (content_size_.width > 0 || content_size_.height > 0)
                      ? content_size_
                      : content_->preferred_size();
    }
    int buttons_width = 0;
    int button_height = 0;
    for (Widget* button : buttons_) {
        Size const size = button->preferred_size();
        buttons_width += size.width;
        button_height = std::max(button_height, size.height);
    }
    if (buttons_.size() > 1) {
        buttons_width += (static_cast<int>(buttons_.size()) - 1) * kButtonGap;
    }
    int const width = std::max(content.width, buttons_width) + 2 * kMargin;
    int const height = kMargin + content.height + kGap + button_height + kMargin;

    DisplayInfo const& display = app_.display_info();
    int x = 0;
    int y = 0;
    if (display.width_px > static_cast<uint64_t>(width)) {
        x = (static_cast<int>(display.width_px) - width) / 2;
    }
    if (display.height_px > static_cast<uint64_t>(height)) {
        y = (static_cast<int>(display.height_px) - height) / 2;
    }
    window_->set_rect({x, y, width, height});
    if (root_ != nullptr) {
        root_->set_rect({0, 0, width, height});
    }

    if (content_ != nullptr) {
        content_->set_rect({kMargin, kMargin, content.width, content.height});
    }
    int bx = width - kMargin - buttons_width;
    int const by = kMargin + content.height + kGap;
    for (Widget* button : buttons_) {
        Size const size = button->preferred_size();
        button->set_rect({bx, by, size.width, button_height});
        bx += size.width + kButtonGap;
    }
}

}  // namespace aegir::trinket
