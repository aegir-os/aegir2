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
#include <aegir/bootstrap.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/heap.h>
#include <aegir/input.h>
#include <aegir/ipc/port.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <sel4/sel4.h>
#include <chrono>
#include <thread>
#include <mutex>

namespace aegir::trinket {

namespace {

/* Static, like the greeter's and the cxx-smoke's: the allocator's untyped
 * table and the scratch window's bookkeeping are tens of kilobytes, and a
 * service's stack is pages. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

/* The console arena's slice is sized from the windows at exec (each backing
 * plus the event ring's page, rounded up to megapages), and the heap claims
 * 8 MiB at the top of the process's own address window. The window the spawn
 * kit grants is 1 GiB (aegir/spawn's process.cc), so the two do not meet. */
constexpr uint64_t kHeapBytes = 8ull << 20;

/* The mapping authority the spawn kit installs: the delegated untyped (page
 * tables and frames are retyped from it), the VSpace root, and the window of
 * free addresses (the give_vspace grant). The pattern is the greeter's and the
 * cxx-smoke's. */
bool adopt_memory() {
    uint64_t untyped_slot = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    uint64_t untyped_physical = 0;
    uint32_t untyped_bits = 0;
    uint64_t untyped_address = 0;
    static_cast<void>(aegir::bootstrap::untyped(&untyped_physical, &untyped_bits,
                                                &untyped_address));

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

    bool ok = aegir::bootstrap::capability("untyped", 7, &untyped_slot) &&
              aegir::bootstrap::capability("vspace", 6, &vspace_slot) &&
              aegir::bootstrap::window(&window_base, &window_bytes) &&
              g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot),
                                      untyped_bits, untyped_physical);
    if (ok) {
        g_objects.adopt_slots(first_free, (1u << 10) - first_free, 0);
        ok = g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                             static_cast<uintptr_t>(window_base),
                             static_cast<uintptr_t>(window_base + window_bytes),
                             &g_objects);
    }
    return ok;
}

}  // namespace

Application* Application::instance_ = nullptr;

Application& Application::create(int argc, char** argv) {
    static_cast<void>(argc);
    static_cast<void>(argv);
    /* The heap must be up before the Application exists: its constructor
     * allocates the theme. This is the cxx-smoke's ordering, and it is why a
     * toolkit app calls create() first. */
    if (!adopt_memory()) {
        aegir::debug_write("trinket: FAIL no untyped, vspace or window\n");
        aegir::halt();
    }
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        aegir::debug_write("trinket: FAIL the heap could not claim the window\n");
        aegir::halt();
    }
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

bool Application::start_console() {
    if (!gui_port_.valid()) return false;

    /* The screen's size is the bound a window's backing may reserve and a
     * resize may reach, so it is asked before the slice is sized
     * (specs/window-manager.md). */
    uint64_t screen_width = 0;
    uint64_t screen_height = 0;
    if (aegir::console::info(gui_port_, &screen_width, &screen_height)) {
        display_info_.width_px = screen_width;
        display_info_.height_px = screen_height;
    }

    /* The slice is sized from the windows: each backing is width * height * 4
     * (the console's B8G8R8X8), plus the event ring's page, rounded up to
     * whole megapages. A greeter's one window is one; a full-screen 1280x800
     * backdrop is two. */
    uint64_t bytes = aegir::console::kEventRingBytes;
    for (Window* win : windows_) {
        if (win->visible()) bytes += win->backing_bytes();
    }
    uint64_t const frame_bytes = 1ull << seL4_LargePageBits;
    bytes = (bytes + frame_bytes - 1) & ~(frame_bytes - 1);
    uint64_t const wanted = bytes >> seL4_LargePageBits;

    uint64_t frame_bits = 0;
    uint64_t frames = 0;
    if (!aegir::console::attach(gui_port_, bytes, &frame_bits, &frames) ||
        frame_bits != seL4_LargePageBits || frames != wanted) {
        return false;
    }

    /* The frames map one after another: the backing is one range. */
    for (uint64_t i = 0; i < frames; ++i) {
        seL4_CPtr const slot = g_objects.alloc_slot();
        if (slot == 0 || !aegir::console::frame(gui_port_, i, slot)) return false;
        void* const mapped = g_scratch.map_large(slot);
        if (mapped == nullptr) return false;
        if (i == 0) {
            slice_ = static_cast<uint8_t*>(mapped);
        } else if (static_cast<uint8_t*>(mapped) != slice_ + i * frame_bytes) {
            return false;
        }
    }
    slice_bytes_ = bytes;

    events_ = g_objects.alloc_slot();
    return events_ != 0 && aegir::console::listen(gui_port_, events_);
}

int Application::exec() {
    running_ = true;
    exit_code_ = 0;

    if (!start_console()) {
        aegir::debug_write("trinket: FAIL the console's channel would not open\n");
        return 1;
    }

    // Show all windows
    for (Window* win : windows_) {
        if (win->visible()) {
            win->create_bureau_window();
        }
    }

    /* Events a window's creation produced -- a requested focus, say -- are
     * dispatched before the client's on_started, so the cue it prints there
     * reflects them rather than racing them. */
    process_events();

    if (on_started) on_started();

    /* Event loop. A server receives on its port and lets the console's event
     * notification -- bound to this thread -- wake the same receive, so one
     * blocked receive serves both (the console's shape); every other app
     * waits on the notification and drains the ring. */
    if (server_.valid()) {
        if (seL4_TCB_BindNotification(aegir::bootstrap::kSlotOwnTcb, events_) !=
            seL4_NoError) {
            aegir::debug_write("trinket: FAIL the event notification would not bind\n");
            return 1;
        }
        seL4_CPtr const port = server_.capability();
        while (running_) {
            seL4_Word badge = 0;
            seL4_MessageInfo_t const info = seL4_Recv(port, &badge);
            if (seL4_MessageInfo_get_length(info) != 0) {
                dispatch_call(info, badge);
            }
            process_events();
            process_timers();
            process_posted_events();
            if (on_poll) on_poll();
        }
    } else {
        while (running_) {
            bool const drained = process_events();
            process_timers();
            process_posted_events();
            if (on_poll) on_poll();
            if (running_ && !drained) {
                seL4_Wait(events_, nullptr);
            }
        }
    }

    // Cleanup
    for (Window* win : windows_) {
        win->destroy_bureau_window();
    }

    return exit_code_;
}

void Application::serve(aegir::ipc::Owner port) {
    server_ = port;
}

void Application::dispatch_call(seL4_MessageInfo_t info, seL4_Word badge) {
    uint32_t const length = static_cast<uint32_t>(seL4_MessageInfo_get_length(info));
    uint32_t const method = length > 0 ? static_cast<uint32_t>(seL4_GetMR(0)) : 0;
    uint32_t const arrived = length > 1 ? length - 1 : 0;
    uint64_t words[aegir::ipc::kMaxWords];
    uint32_t const taken = arrived < aegir::ipc::kMaxWords ? arrived : aegir::ipc::kMaxWords;
    for (uint32_t i = 0; i < taken; ++i) {
        words[i] = seL4_GetMR(1 + i);
    }
    uint64_t reply[aegir::ipc::kMaxWords];
    uint32_t reply_count = 0;
    if (on_call) {
        reply_count = on_call(method, words, arrived, badge, reply, aegir::ipc::kMaxWords);
    }
    server_.reply_words(reply, reply_count);
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

Font* Application::default_font() {
    if (!default_font_) {
        default_font_ = load_builtin_font("Terminus", 12);
        if (!default_font_) {
            aegir::debug_write("Warning: Could not load default font\n");
        }
    }
    return default_font_.get();
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

uint64_t Application::claim_backing(uint64_t bytes) {
    if (slice_ == nullptr || bytes == 0) return ~0ull;
    /* The ring is the slice's last page and belongs to the console. */
    uint64_t const limit = slice_bytes_ - aegir::console::kEventRingBytes;
    if (backing_next_ + bytes > limit) return ~0ull;
    uint64_t const offset = backing_next_;
    backing_next_ += bytes;
    return offset;
}

std::unique_ptr<Font> Application::load_font(std::string_view family, int size_pts) {
    // Load from resources/fonts/
    // For now, use builtin
    return load_builtin_font(family, size_pts);
}

std::unique_ptr<Font> Application::load_builtin_font(std::string_view name, int size_pts) {
    if (name == "Terminus") {
        return Font::load_terminus(size_pts, display_info_.scale);
    }
    return nullptr;
}

std::string Application::resource_path(std::string_view relative) const {
    // Resources are copied to build directory
    return std::string("resources/") + std::string(relative);
}

bool Application::process_events() {
    if (events_ == 0 || slice_ == nullptr) return false;
    volatile uint64_t* const ring = aegir::console::event_ring(slice_, slice_bytes_);
    bool took = false;
    uint64_t event = 0;
    uint64_t window = 0;
    while (aegir::console::ring_take(ring, &event, &window)) {
        took = true;
        dispatch_gui_event(event, window);
    }
    return took;
}

void Application::dispatch_gui_event(uint64_t event, uint64_t window) {
    uint16_t const type = aegir::input::event_type(event);
    uint32_t const value = aegir::input::event_value(event);
    for (Window* win : windows_) {
        if (win->console_window_id() != window) continue;
        if (type == aegir::console::kEventFocus) {
            if (value == 1) {
                win->on_focus_gained();
            } else {
                win->on_focus_lost();
            }
        } else if (type == aegir::console::kEventKey) {
            win->dispatch_key(event);
        } else if (type == aegir::console::kEventPointer) {
            win->dispatch_pointer(event);
        }
        return;
    }
}

void Application::process_timers() {
    /* A timer is the only thing that needs the clock, and there is no
     * user-accessible monotonic clock yet: musl's clock_gettime is refused
     * (aegir-heap answers memory syscalls), and with exceptions off libc++'s
     * steady_clock terminates on the error. So the loop asks only when a
     * timer is actually pending -- tier 1 schedules none. A real clock source
     * arrives with the threading milestone (specs/trinket.md). */
    {
        std::lock_guard<std::mutex> lock(posted_mutex_);
        bool timed = false;
        for (const PostedEvent& event : posted_events_) {
            if (event.due_time > 0) {
                timed = true;
                break;
            }
        }
        if (!timed) return;
    }

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
