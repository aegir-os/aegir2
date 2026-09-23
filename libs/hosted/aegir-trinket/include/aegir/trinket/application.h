/*
 * Trinket Application - main entry point and event loop.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_TRINKET_APPLICATION_H
#define AEGIR_TRINKET_APPLICATION_H

#include <aegir/trinket/point.h>
#include <aegir/trinket/font.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/worker.h>
#include <aegir/console.h>
#include <aegir/ipc/port.h>
#include <functional>
#include <memory>
#include <vector>

namespace aegir::trinket {

class Window;

struct DisplayInfo {
    uint64_t width_px = 0;
    uint64_t height_px = 0;
    uint32_t physical_width_mm = 0;
    uint32_t physical_height_mm = 0;
    float scale = 1.0f;       // DPI / 96.0
    float dpi = 96.0f;
};

class Application {
public:
    static Application* instance();
    static Application& create(int argc, char** argv);
    ~Application();

    // Non-copyable, movable
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Main event loop
    int exec();
    void quit(int exit_code = 0);
    int exit_code() const { return exit_code_; }

    // Called by exec once the windows are created and painted: a client's cue
    // that its form is on the screen.
    std::function<void()> on_started;

    // The screen's owner appeared (true) or left (false): the console's nudge
    // to every listening client when a backdrop window is created or
    // destroyed. A client that may be focused before the bureau exists
    // registers here, once it is up (specs/workbench.md).
    std::function<void(bool up)> on_screen_owner;

    // A server's call handler, when serve() set a port: `method` and the
    // `count` words that rode with it, answered with as many reply words as
    // the handler returns. `badge` is the caller's, as the kernel reports it.
    // `cap_arrived` says a capability rode with the call and is in the scratch
    // receive slot (ipc::take_received_cap is how the handler moves it out).
    std::function<uint32_t(uint32_t method, uint64_t const *words, uint32_t count,
                           seL4_Word badge, bool cap_arrived, uint64_t *reply,
                           uint32_t capacity)> on_call;

    // Called after each drain and before the loop waits again: a client with
    // an out-of-band signal to poll (the bureau.menu doorbell) does it here.
    std::function<void()> on_poll;

    // Theme
    void set_theme(std::unique_ptr<Theme> theme);
    Theme& theme() const { return *theme_; }
    void set_default_font(std::unique_ptr<Font> font);
    // The application's font, loaded from the embedded default the first time
    // a widget asks: a client whose windows draw no text (the bureau's
    // backdrop) never pays for the atlas.
    Font* default_font();

    // Locale
    void set_locale(const Locale& locale);
    const Locale& locale() const { return Locale::global(); }

    // Main thread event posting
    void post_event(std::function<void()>&& fn);
    void schedule_timer(int64_t delay_ms, std::function<void()>&& fn);

    // Worker pool
    WorkerPool& workers() { return workers_; }

    // Window management
    void register_window(Window* window);
    void unregister_window(Window* window);
    std::vector<Window*> windows() const;

    // Display info from GPU driver
    const DisplayInfo& display_info() const { return display_info_; }

    // Console GUI port
    aegir::ipc::Consumer& gui_port() { return gui_port_; }
    void set_gui_port(aegir::ipc::Consumer port);

    // Serve a port: exec() then receives on it, and the console's event
    // notification -- bound to this thread -- wakes the same receive. A
    // single-threaded server cannot wait on two objects at once, so it takes
    // the console's own shape (specs/workbench.md).
    void serve(aegir::ipc::Owner port);

    // A CSpace slot for a capability the app installs -- a served port's
    // transferred cap, say. Slots are the process's and one is never freed
    // (aegir-mem's allocator), which a registry that reuses a client's slot
    // across re-registrations respects.
    seL4_CPtr alloc_slot();

    // A signal-only copy of the event notification, in `target` (a slot from
    // alloc_slot()): what a client hands a server that must ring its doorbell
    // (specs/workbench.md's bureau.menu). The console's listen mint carries
    // Write, so the copy can be made; the signal only wakes this app.
    bool mint_event_notification(seL4_CPtr target);

    // The mapped console slice and a window's backing within it. A Window
    // claims a region once and keeps its offset; the region stops short of the
    // event ring in the slice's last page.
    uint8_t* slice() const { return slice_; }
    uint64_t slice_bytes() const { return slice_bytes_; }
    uint64_t claim_backing(uint64_t bytes);

    // Font loading from resources
    std::unique_ptr<Font> load_font(std::string_view family, int size_pts);
    std::unique_ptr<Font> load_builtin_font(std::string_view name, int size_pts);

    // Resource paths
    std::string resource_path(std::string_view relative) const;

private:
    friend class Window;

    Application();
    bool start_console();
    bool process_events();
    void process_timers();
    void process_posted_events();
    void dispatch_gui_event(uint64_t event, uint64_t window);
    void dispatch_call(seL4_MessageInfo_t info, seL4_Word badge);
    int64_t now_ms() const;

    static Application* instance_;
    int exit_code_ = 0;
    bool running_ = false;

    std::unique_ptr<Theme> theme_;
    std::unique_ptr<Font> default_font_;
    WorkerPool workers_;
    std::vector<Window*> windows_;
    DisplayInfo display_info_;
    aegir::ipc::Consumer gui_port_;
    aegir::ipc::Owner server_;

    // The console state: the slice mapped into this process's window, the
    // event notification, and the backing allocator's cursor.
    uint8_t* slice_ = nullptr;
    uint64_t slice_bytes_ = 0;
    uint64_t backing_next_ = 0;
    seL4_CPtr events_ = 0;

    struct PostedEvent {
        std::function<void()> fn;
        int64_t due_time = 0;  // 0 = immediate
    };
    std::vector<PostedEvent> posted_events_;
    std::mutex posted_mutex_;
};

} // namespace aegir::trinket

#endif // AEGIR_TRINKET_APPLICATION_H