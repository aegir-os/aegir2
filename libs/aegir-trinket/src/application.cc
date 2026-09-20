/*
 * Trinket Application implementation.
 */

#include <aegir/trinket/application.h>
#include <aegir/trinket/canvas.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/font.h>
#include <aegir/trinket/locale.h>
#include <aegir/trinket/translation.h>
#include <aegir/trinket/window.h>
#include <aegir/console.h>
#include <aegir/framebuffer.h>
#include <aegir/ipc/port.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>
#include <chrono>
#include <thread>
#include <mutex>

namespace aegir::trinket {

Application* Application::instance_ = nullptr;

Application& Application::create(int argc, char** argv) {
    static_cast<void>(argc);
    static_cast<void>(argv);
    static Application app;
    instance_ = &app;
    return app;
}

Application::Application()
    : theme_(Theme::create_xen(1.0f)),
      workers_(2) {
    instance_ = this;
}

Application::~Application() {
    instance_ = nullptr;
}

Application* Application::instance() {
    return instance_;
}

int Application::exec() {
    running_ = true;
    exit_code_ = 0;

    // Initialize display info
    init_display_info();

    // Load default font
    default_font_ = load_builtin_font("Terminus", 12);
    if (!default_font_) {
        // Fallback
        aegir::debug_write("Warning: Could not load default font\n");
    }

    // Show all windows
    for (Window* win : windows_) {
        if (win->visible()) {
            win->create_bureau_window();
        }
    }

    // Event loop
    while (running_) {
        process_events();
        process_timers();
        process_posted_events();
    }

    // Cleanup
    for (Window* win : windows_) {
        win->destroy_bureau_window();
    }

    return exit_code_;
}

void Application::quit(int exit_code) {
    exit_code_ = exit_code;
    running_ = false;
}

void Application::set_theme(std::unique_ptr<Theme> theme) {
    theme_ = std::move(theme);
}

void Application::set_default_font(std::unique_ptr<Font> font) {
    default_font_ = std::move(font);
}

void Application::set_locale(const Locale& locale) {
    Locale::set_global(locale);
    // Reload translations for new locale
    // TODO: Load translation files
}

void Application::post_event(std::function<void()>&& fn) {
    std::lock_guard<std::mutex> lock(posted_mutex_);
    posted_events_.push_back({std::move(fn), 0});
}

void Application::schedule_timer(int64_t delay_ms, std::function<void()>&& fn) {
    std::lock_guard<std::mutex> lock(posted_mutex_);
    posted_events_.push_back({std::move(fn), now_ms() + delay_ms});
}

void Application::register_window(Window* window) {
    windows_.push_back(window);
}

void Application::unregister_window(Window* window) {
    auto it = std::find(windows_.begin(), windows_.end(), window);
    if (it != windows_.end()) {
        windows_.erase(it);
    }
}

std::vector<Window*> Application::windows() const {
    return windows_;
}

void Application::set_gui_port(aegir::ipc::Consumer port) {
    gui_port_ = port;
}

std::unique_ptr<Font> Application::load_font(std::string_view family, int size_pts) {
    // Load from resources/fonts/
    // For now, use builtin
    return load_builtin_font(family, size_pts);
}

std::unique_ptr<Font> Application::load_builtin_font(std::string_view name, int size_pts) {
    static_cast<void>(size_pts);  // font resources are a later milestone
    if (name == "Terminus") {
        // TODO: Load from resources/fonts/terminus/
        // For now, return null - will be implemented when font loading works
        return nullptr;
    }
    return nullptr;
}

std::string Application::resource_path(std::string_view relative) const {
    // Resources are copied to build directory
    return std::string("resources/") + std::string(relative);
}

void Application::init_display_info() {
    // Query GPU driver for display info
    if (gui_port_.valid()) {
        uint64_t in[aegir::framebuffer::kInfoWords];
        aegir::ipc::WordsReply info = gui_port_.call_words(
            aegir::framebuffer::kMethodInfo, nullptr, 0, in,
            aegir::framebuffer::kInfoWords);

        if (info.error == 0 && info.count == aegir::framebuffer::kInfoWords) {
            display_info_.width_px = in[0];
            display_info_.height_px = in[1];
            display_info_.physical_width_mm = in[4];
            display_info_.physical_height_mm = in[5];

            // Calculate DPI
            if (display_info_.physical_width_mm > 0) {
                display_info_.dpi = display_info_.width_px * 25.4 / display_info_.physical_width_mm;
            } else {
                display_info_.dpi = 96.0f;
            }
            display_info_.scale = display_info_.dpi / 96.0f;
        }
    }

    // Apply scale to theme
    theme_ = Theme::create_xen(display_info_.scale);
}

void Application::process_events() {
    /* The console's events do not arrive by receiving on its port: a window's
     * events come over a notification and a ring, which is the window arc's to
     * drain (specs/console.md). The port this holds is call-only. */
}

void Application::process_timers() {
    int64_t now = now_ms();
    std::vector<PostedEvent> remaining;
    remaining.reserve(posted_events_.size());

    for (auto& event : posted_events_) {
        if (event.due_time > 0 && event.due_time <= now) {
            if (event.fn) event.fn();
        } else {
            remaining.push_back(std::move(event));
        }
    }

    std::lock_guard<std::mutex> lock(posted_mutex_);
    posted_events_ = std::move(remaining);
}

void Application::process_posted_events() {
    std::vector<PostedEvent> to_run;
    {
        std::lock_guard<std::mutex> lock(posted_mutex_);
        to_run.reserve(posted_events_.size());
        for (auto& event : posted_events_) {
            if (event.due_time == 0) {
                to_run.push_back(std::move(event));
            }
        }
        // Keep non-due events
        std::vector<PostedEvent> remaining;
        remaining.reserve(posted_events_.size());
        for (auto& event : posted_events_) {
            if (event.due_time != 0) {
                remaining.push_back(std::move(event));
            }
        }
        posted_events_ = std::move(remaining);
    }

    for (auto& event : to_run) {
        if (event.fn) event.fn();
    }
}

int64_t Application::now_ms() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace aegir::trinket