/*
 * aegir-console: the display, the pointer, and the windows.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The one service that touches the display and the input devices
 * (specs/console.md). This first slice owns the hardware and the screen:
 * it opens the gpu and the three HID drivers through the registry -- so
 * nothing else does -- maps the gpu's pixel window into its own address
 * space (the manifest's `maps` grant: its VSpace root and a window of its
 * own addresses), paints the backdrop, and flushes. The window protocol
 * and the event channels arrive behind this; the port already answers,
 * and what it does not know it says nothing to, the version rule.
 */

#include <aegir/bootstrap.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/framebuffer.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/vspace.h>
#include <aegir/registry.h>
#include <sel4/sel4.h>
#include <stdint.h>
#include <string.h>

namespace {

void write_line(char const *text) noexcept
{
    aegir::debug_write("      console: ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

/* The backdrop: Workbench blue, one word a pixel in the driver's B8G8R8X8
 * (aegir/framebuffer.h). */
constexpr uint32_t kBackdrop = 0x000055AA;

/* Whether a button-down on a backdrop takes the focus. The screen bar is the
 * bureau's backdrop's own top (specs/workbench.md), and clicking it must not
 * deactivate the window whose menus it shows -- the Amiga keeps the screen bar
 * above the windows. A ClickToFocus-style commodity is what will own the
 * choice; until then this is the one policy point. */
constexpr bool kBackdropTakesFocus = false;

/* Static, not local, and that is not a style choice: an Allocator carries
 * the tables of what it handed out, and a service's stack is pages, not
 * tables (the device manager says the same of its own). */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

/* The screen the window protocol composites into: the gpu port, its
 * mapped window, and the geometry `info` answered. */
aegir::ipc::Consumer g_gpu(0);
uint8_t *g_screen = nullptr;
uint64_t g_width = 0;
uint64_t g_height = 0;
uint64_t g_stride = 0;

/* Push a changed rectangle of the screen to the display: the driver transfers
 * and flushes only this region (aegir/framebuffer.h). The whole screen per
 * event -- 4 MiB copied to the device -- was what made typing and dragging
 * crawl. */
void flush(uint64_t x, uint64_t y, uint64_t width, uint64_t height) noexcept
{
    uint64_t rect[aegir::framebuffer::kFlushWords] = {x, y, width, height};
    (void)g_gpu.call_words(aegir::framebuffer::kMethodFlush, rect,
                           aegir::framebuffer::kFlushWords, nullptr, 0);
}

/* A client's slice of the arena: megapage frames retyped from a child
 * untyped of the slice's own, so a reap revokes exactly one client's
 * pixels (authority.md's retained-copy path). The console maps the carved
 * set -- it composites through it -- and hands copies out of the pristine
 * mint set, made before any mapping, because a mapped cap's copies are
 * pinned to its ASID and useless to another address space
 * (kernel/src/arch/riscv/kernel/vspace.c:869-878). Every cap is named by
 * the slot the allocator handed out for it -- the carved frames at
 * slots[0..frames), their pristine mints at slots[frames..2*frames) --
 * because deriving one object's slot from another's by adding is
 * adjacency reasoning, and the allocator interleaves (frame, mint) pairs. */
struct Slice {
    uint64_t badge;
    seL4_CPtr untyped;  /* the slice's own: revoking it reclaims the whole */
    uint64_t frames;
    uintptr_t base; /* where the console reads the slice */
    seL4_CPtr events; /* the client's event notification, ours to signal */
    bool listening;   /* the notification's mint went out (one per client) */
    Slice *next;
    seL4_CPtr slots[]; /* carved at [f], pristine mints at [frames + f] */
};

/* A window: a rectangle on the screen and where its pixels live in the
 * owner's slice. The list is in z order, bottom first; create appends, so
 * new windows sit on top -- except a backdrop window, which enters at the
 * bottom and, with no raise in the model, stays there. A window is
 * invisible until its first damage -- the backing is the client's to
 * paint first, and retyped frames arrive dirty, so compositing one
 * earlier would show memory, not pixels. */
struct Window {
    uint64_t id;
    uint64_t owner; /* the badge create_window arrived with */
    uint64_t x;
    uint64_t y;
    uint64_t width;
    uint64_t height;
    uint64_t offset; /* the backing's offset within the owner's slice */
    bool shown;      /* the first damage happened */
    bool backdrop;   /* kWindowBackdrop at create: the bottom of the z-order */
    /* The owner's slice, resolved once at create: the composite reads it for
     * every pixel it covers, and a linear find_slice there was the walk that
     * made dragging and typing crawl. `reap` destroys a badge's windows
     * before it revokes the slice, so this stays valid while the window is
     * in the list. */
    Slice *slice;
    Window *next;
};

Slice *g_slices = nullptr;
Window *g_windows = nullptr;
uint64_t g_next_id = 0;

Slice *find_slice(uint64_t badge) noexcept
{
    for (Slice *s = g_slices; s != nullptr; s = s->next) {
        if (s->badge == badge) {
            return s;
        }
    }
    return nullptr;
}

Window *find_window(uint64_t id) noexcept
{
    for (Window *w = g_windows; w != nullptr; w = w->next) {
        if (w->id == id) {
            return w;
        }
    }
    return nullptr;
}

/* ---- Input routing (specs/console.md's focus and the pointer) ---- */

/* The HID devices in the order their mints' badge bits name them: a
 * wakeup's bit (1 << d) is this device's queue having moved. */
aegir::ipc::Consumer g_hid[3];
constexpr uint32_t kHidKbd = 0;
constexpr uint32_t kHidMouse = 1;
constexpr uint32_t kHidTablet = 2;

/* The keymap is the system's, US layout v1 (specs/console.md): indexed by
 * the raw code as it arrives -- Linux's KEY_*, which virtio-input carries
 * unchanged -- with a zero where a code has no character. First the
 * unshifted character, then the shifted. */
constexpr char kKeymap[58][2] = {
    {0, 0},          {0, 0},          {'1', '!'},  {'2', '@'},  /* 0-3 */
    {'3', '#'},      {'4', '$'},      {'5', '%'},  {'6', '^'},  /* 4-7 */
    {'7', '&'},      {'8', '*'},      {'9', '('},  {'0', ')'},  /* 8-11 */
    {'-', '_'},      {'=', '+'},      {'\b', '\b'}, {'\t', '\t'}, /* 12-15 */
    {'q', 'Q'},      {'w', 'W'},      {'e', 'E'},  {'r', 'R'},  /* 16-19 */
    {'t', 'T'},      {'y', 'Y'},      {'u', 'U'},  {'i', 'I'},  /* 20-23 */
    {'o', 'O'},      {'p', 'P'},      {'[', '{'},  {']', '}'},  /* 24-27 */
    {'\n', '\n'},    {0, 0},          {'a', 'A'},  {'s', 'S'},  /* 28-31 */
    {'d', 'D'},      {'f', 'F'},      {'g', 'G'},  {'h', 'H'},  /* 32-35 */
    {'j', 'J'},      {'k', 'K'},      {'l', 'L'},  {';', ':'},  /* 36-39 */
    {'\'', '"'},     {'`', '~'},      {0, 0},      {'\\', '|'}, /* 40-43 */
    {'z', 'Z'},      {'x', 'X'},      {'c', 'C'},  {'v', 'V'},  /* 44-47 */
    {'b', 'B'},      {'n', 'N'},      {'m', 'M'},  {',', '<'},  /* 48-51 */
    {'.', '>'},      {'/', '?'},      {0, 0},      {0, 0},      /* 52-55 */
    {0, 0},          {' ', ' '},                                  /* 56-57 */
};
constexpr uint16_t kKeyLeftCtrl = 29;
constexpr uint16_t kKeyRightCtrl = 97;
constexpr uint16_t kKeyLeftShift = 42;
constexpr uint16_t kKeyRightShift = 54;
constexpr uint16_t kKeyLeftAlt = 56;
constexpr uint16_t kKeyRightAlt = 100;
constexpr uint16_t kKeyLeftMeta = 125;
constexpr uint16_t kKeyRightMeta = 126;

/* The modifier keys are the console's state: the keymap's shifted column
 * needs shift, and a client that maps the raw code needs all four. Each is
 * set on its press and cleared on its release, and the delivered event
 * carries the state after the key was applied. */
bool g_shift = false;
bool g_control = false;
bool g_alt = false;
bool g_super = false;

/* The pointer: tracked by the console (the tablet's absolute events are
 * the natural feed, the mouse's relative ones integrate to the same
 * point), focused by click (Amiga semantics: a button-down focuses, and
 * does not raise), and grabbed for the length of a drag. */
uint64_t g_pointer_x = 0;
uint64_t g_pointer_y = 0;
Window *g_focused = nullptr;
Window *g_grab = nullptr;

/* Motion is coalesced: a drain may carry several reports, and only the last
 * position matters. The cursor is erased at the first, drawn at the flush,
 * and one event is delivered -- a client resizes or moves once per drain,
 * not once per report, which is what a fast pointer outran. */
bool g_motion_pending = false;
uint64_t g_motion_old_x = 0;
uint64_t g_motion_old_y = 0;

/* The topmost window covering a point, or none. The list is bottom first,
 * so the last coverer is the answer. */
Window *window_at(uint64_t x, uint64_t y) noexcept
{
    Window *found = nullptr;
    for (Window *w = g_windows; w != nullptr; w = w->next) {
        if (x >= w->x && x < w->x + w->width && y >= w->y && y < w->y + w->height) {
            found = w;
        }
    }
    return found;
}

/* Append an event to a client's ring and signal. A full ring drops: the
 * console never blocks on a client that stopped reading. */
void deliver_to(Slice *slice, uint16_t type, uint16_t code, uint32_t value,
                uint64_t window) noexcept
{
    if (slice == nullptr || slice->events == 0) {
        return;
    }
    volatile uint64_t *const ring = aegir::console::event_ring(
        reinterpret_cast<uint8_t *>(slice->base), slice->frames << seL4_LargePageBits);
    uint64_t const write = ring[0];
    if (write - ring[1] >= aegir::console::kEventRingEntries) {
        return;
    }
    volatile uint64_t *const entry =
        ring + 2 + (write % aegir::console::kEventRingEntries) * 2;
    entry[0] = aegir::input::pack_event(type, code, value);
    entry[1] = window;
    ring[0] = write + 1;
    seL4_Signal(slice->events);
}

void deliver(uint64_t owner, uint16_t type, uint16_t code, uint32_t value,
             uint64_t window) noexcept
{
    deliver_to(find_slice(owner), type, code, value, window);
}

/* A backdrop -- the screen's owner, the bureau -- appeared or left: every
 * listening client hears it. A client that was focused before the bureau
 * existed uses this nudge to register the moment it is up
 * (specs/workbench.md); the console is the one service always there to send
 * it. A full ring drops, as ever. */
void announce_screen_owner(uint64_t value) noexcept
{
    for (Slice *slice = g_slices; slice != nullptr; slice = slice->next) {
        if (slice->listening) {
            deliver_to(slice, aegir::console::kEventScreenOwner, 0,
                       static_cast<uint32_t>(value), 0);
        }
    }
}

/* The cursor is software (specs/console.md): a 16x16 arrow composited over
 * the output, with what is under it saved and restored around every move and
 * repaint. It is white with a black border one pixel around it, so it stays
 * visible on a white surface. The touched box is the arrow plus one pixel
 * each way (the border), which is what the save-under holds. */
constexpr uint64_t kCursorSize = 16;
/* A solid arrow: the fill is set, and the outline is derived from it (a set
 * pixel with an unset neighbour is the border). An outline bitmap would leave
 * the interior transparent, which is the see-through centre this replaced. */
constexpr uint16_t kCursorShape[kCursorSize] = {
    0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFC0, 0xFFE0, 0xFFF0, 0xF000, 0xF000, 0xF000, 0xF000};
constexpr uint64_t kCursorSpan = kCursorSize + 2;
uint32_t g_cursor_under[kCursorSpan * kCursorSpan];
int64_t g_cursor_origin_x = 0;
int64_t g_cursor_origin_y = 0;
bool g_cursor_drawn = false;

bool cursor_set(int mx, int my) noexcept
{
    if (mx < 0 || my < 0 || mx >= static_cast<int>(kCursorSize) ||
        my >= static_cast<int>(kCursorSize)) {
        return false;
    }
    return (kCursorShape[my] & (0x8000u >> mx)) != 0;
}

/* The pixel at arrow-local (mx, my): the fill is white and the edge one pixel
 * around it is black, so a white pointer stays visible on a white surface.
 * 0xFFFFFFFF means leave the screen alone. */
uint32_t cursor_pixel(int mx, int my) noexcept
{
    if (cursor_set(mx, my)) {
        return 0x00FFFFFFu;
    }
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if ((dx != 0 || dy != 0) && cursor_set(mx + dx, my + dy)) {
                return 0x00000000u;
            }
        }
    }
    return 0xFFFFFFFFu;
}

void cursor_draw() noexcept
{
    g_cursor_origin_x = static_cast<int64_t>(g_pointer_x) - 1;
    g_cursor_origin_y = static_cast<int64_t>(g_pointer_y) - 1;
    for (uint64_t dy = 0; dy < kCursorSpan; ++dy) {
        for (uint64_t dx = 0; dx < kCursorSpan; ++dx) {
            int64_t const sx = g_cursor_origin_x + static_cast<int64_t>(dx);
            int64_t const sy = g_cursor_origin_y + static_cast<int64_t>(dy);
            uint64_t const index = dy * kCursorSpan + dx;
            if (sx < 0 || sy < 0 || sx >= static_cast<int64_t>(g_width) ||
                sy >= static_cast<int64_t>(g_height)) {
                g_cursor_under[index] = 0;
                continue;
            }
            auto *const out =
                reinterpret_cast<uint32_t *>(g_screen + sy * g_stride);
            g_cursor_under[index] = out[sx];
            uint32_t const colour = cursor_pixel(static_cast<int>(dx) - 1,
                                                 static_cast<int>(dy) - 1);
            if (colour != 0xFFFFFFFFu) {
                out[sx] = colour;
            }
        }
    }
    g_cursor_drawn = true;
}

void cursor_erase() noexcept
{
    if (!g_cursor_drawn) {
        return;
    }
    for (uint64_t dy = 0; dy < kCursorSpan; ++dy) {
        for (uint64_t dx = 0; dx < kCursorSpan; ++dx) {
            int64_t const sx = g_cursor_origin_x + static_cast<int64_t>(dx);
            int64_t const sy = g_cursor_origin_y + static_cast<int64_t>(dy);
            if (sx < 0 || sy < 0 || sx >= static_cast<int64_t>(g_width) ||
                sy >= static_cast<int64_t>(g_height)) {
                continue;
            }
            auto *out = reinterpret_cast<uint32_t *>(g_screen + sy * g_stride);
            out[sx] = g_cursor_under[dy * kCursorSpan + dx];
        }
    }
    g_cursor_drawn = false;
}

/* Deliver the coalesced motion: the cursor is drawn at its final position,
 * the old and new cursor rectangles are flushed, and one event goes out.
 * Called before any event that is not motion, and at the end of a drain, so
 * the order a client sees is preserved. */
void flush_motion() noexcept
{
    if (!g_motion_pending) {
        return;
    }
    g_motion_pending = false;
    cursor_draw();
    uint64_t const min_x =
        g_motion_old_x < g_pointer_x ? g_motion_old_x : g_pointer_x;
    uint64_t const min_y =
        g_motion_old_y < g_pointer_y ? g_motion_old_y : g_pointer_y;
    uint64_t const max_x =
        (g_motion_old_x > g_pointer_x ? g_motion_old_x : g_pointer_x) + kCursorSpan;
    uint64_t const max_y =
        (g_motion_old_y > g_pointer_y ? g_motion_old_y : g_pointer_y) + kCursorSpan;
    uint64_t const left = min_x > 0 ? min_x - 1 : 0;
    uint64_t const top = min_y > 0 ? min_y - 1 : 0;
    flush(left, top, max_x - left, max_y - top);
    Window *const target =
        g_grab != nullptr ? g_grab : window_at(g_pointer_x, g_pointer_y);
    if (target != nullptr) {
        /* During a grab the window itself is moving, so window-local
         * coordinates have no stable frame: an event queued while the window
         * was somewhere else reads against the wrong origin, and a drag that
         * used it overshot ("threw") the window. The grab holder is sent the
         * pointer's *screen* position instead; an ungrab is window-local, as
         * it always was (specs/window-manager.md). */
        uint32_t const where =
            g_grab != nullptr
                ? static_cast<uint32_t>(g_pointer_x | (g_pointer_y << 16))
                : static_cast<uint32_t>((g_pointer_x - target->x) |
                                        ((g_pointer_y - target->y) << 16));
        deliver(target->owner, aegir::console::kEventPointer, 0, where, target->id);
    }
}

/* A button-down hit-tests, focuses -- and does not raise -- and grabs for
 * the drag; the up goes to the grab-held window. */
void pointer_button(uint16_t code, uint32_t state) noexcept
{
    Window *const under = window_at(g_pointer_x, g_pointer_y);
    if (state != 0) {
        g_grab = under;
        /* A backdrop is not a focus target: the screen bar is the bureau's
         * backdrop's own top, and clicking it must not take the focus from the
         * window whose menus it shows (kBackdropTakesFocus, above). */
        bool const takes_focus =
            under == nullptr || !under->backdrop || kBackdropTakesFocus;
        if (under != g_focused && takes_focus) {
            if (g_focused != nullptr) {
                deliver(g_focused->owner, aegir::console::kEventFocus, 0, 0,
                        g_focused->id);
            }
            g_focused = under;
            if (under != nullptr) {
                deliver(under->owner, aegir::console::kEventFocus, 0, 1, under->id);
            }
        }
    }
    Window *const target = g_grab != nullptr ? g_grab : under;
    if (target != nullptr) {
        deliver(target->owner, aegir::console::kEventPointer,
                static_cast<uint16_t>(code | (state == 0 ? aegir::console::kButtonRelease
                                                          : 0)),
                static_cast<uint32_t>((g_pointer_x - target->x) |
                                      ((g_pointer_y - target->y) << 16)),
                target->id);
    }
    if (state == 0) {
        g_grab = nullptr;
    }
}

void key_event(uint16_t code, uint32_t state) noexcept
{
    bool const pressed = state != 0;
    switch (code) {
    case kKeyLeftShift:
    case kKeyRightShift:
        g_shift = pressed;
        break;
    case kKeyLeftCtrl:
    case kKeyRightCtrl:
        g_control = pressed;
        break;
    case kKeyLeftAlt:
    case kKeyRightAlt:
        g_alt = pressed;
        break;
    case kKeyLeftMeta:
    case kKeyRightMeta:
        g_super = pressed;
        break;
    default:
        break;
    }
    if (g_focused == nullptr) {
        return;
    }
    uint32_t const translated =
        code < 58 ? static_cast<uint8_t>(kKeymap[code][g_shift ? 1 : 0]) : 0;
    uint32_t modifiers = 0;
    if (g_shift) {
        modifiers |= aegir::console::kKeyShift;
    }
    if (g_control) {
        modifiers |= aegir::console::kKeyControl;
    }
    if (g_alt) {
        modifiers |= aegir::console::kKeyAlt;
    }
    if (g_super) {
        modifiers |= aegir::console::kKeySuper;
    }
    deliver(g_focused->owner, aegir::console::kEventKey, code,
            translated | (pressed ? aegir::console::kKeyPressed : 0) | modifiers,
            g_focused->id);
}

/* One event from a device, routed. */
void route(uint32_t device, uint64_t word) noexcept
{
    uint16_t const type = aegir::input::event_type(word);
    uint16_t const code = aegir::input::event_code(word);
    uint32_t const value = aegir::input::event_value(word);
    if (device == kHidKbd && type == aegir::input::kEvKey) {
        flush_motion();
        key_event(code, value);
    } else if (device == kHidMouse && type == aegir::input::kEvRel) {
        /* Relative motion integrates to the pointer, clamped to the screen.
         * The first motion of a drain takes the cursor down and remembers
         * where it was; the drain's end draws it once at the final spot. */
        if (!g_motion_pending) {
            cursor_erase();
            g_motion_old_x = g_pointer_x;
            g_motion_old_y = g_pointer_y;
            g_motion_pending = true;
        }
        int64_t const next =
            static_cast<int64_t>(code == aegir::input::kAxisX ? g_pointer_x
                                                              : g_pointer_y) +
            static_cast<int32_t>(value);
        uint64_t const limit =
            code == aegir::input::kAxisX ? g_width - 1 : g_height - 1;
        uint64_t const clamped =
            next < 0 ? 0 : static_cast<uint64_t>(next) > limit ? limit
                                                               : static_cast<uint64_t>(next);
        if (code == aegir::input::kAxisX) {
            g_pointer_x = clamped;
        } else {
            g_pointer_y = clamped;
        }
    } else if (device == kHidTablet && type == aegir::input::kEvAbs) {
        /* Absolute, in the axis's own units (0..32767): scaled to the
         * screen, the numbers pass through unchanged from the injector.
         * Cursor down first, as for the relative axis above. */
        if (!g_motion_pending) {
            cursor_erase();
            g_motion_old_x = g_pointer_x;
            g_motion_old_y = g_pointer_y;
            g_motion_pending = true;
        }
        if (code == aegir::input::kAxisX) {
            g_pointer_x = (static_cast<uint64_t>(value) * g_width) >> 15;
        } else {
            g_pointer_y = (static_cast<uint64_t>(value) * g_height) >> 15;
        }
    } else if (device != kHidKbd && type == aegir::input::kEvKey &&
               code >= aegir::input::kBtnLeft && code <= aegir::input::kBtnMiddle) {
        /* The click is delivered where the pointer stands now, so any motion
         * waiting goes first. */
        flush_motion();
        pointer_button(code, value);
    }
    /* EV_SYN ends a moment's worth of events; delivered as they come. */
}

/* Drain one device's queue -- poll, then next only when an event waits,
 * so the reply is never held -- and route what came. */
void drain(uint32_t device) noexcept
{
    for (;;) {
        uint64_t in[1];
        aegir::ipc::WordsReply const polled =
            g_hid[device].call_words(aegir::input::kMethodPoll, nullptr, 0, in, 1);
        if (polled.error != 0 || polled.count != 1 || in[0] == 0) {
            break;
        }
        aegir::ipc::WordsReply const next =
            g_hid[device].call_words(aegir::input::kMethodNext, nullptr, 0, in, 1);
        if (next.error != 0 || next.count != 1) {
            break;
        }
        route(device, in[0]);
    }
    /* Whatever motion the drain carried goes out as one event. */
    flush_motion();
}

/* Composite a screen rectangle: every pixel is the topmost window covering
 * it, or the backdrop. The windows are walked bottom to top, so the last
 * coverer wins. */
void composite_rect(uint64_t sx, uint64_t sy, uint64_t width,
                    uint64_t height) noexcept
{
    if (sx >= g_width || sy >= g_height) {
        return;
    }
    uint64_t const ex = sx + width > g_width ? g_width : sx + width;
    uint64_t const ey = sy + height > g_height ? g_height : sy + height;
    for (uint64_t yy = sy; yy < ey; ++yy) {
        auto *out =
            reinterpret_cast<uint32_t *>(g_screen + yy * g_stride);
        for (uint64_t xx = sx; xx < ex; ++xx) {
            uint32_t pixel = kBackdrop;
            for (Window const *w = g_windows; w != nullptr; w = w->next) {
                if (!w->shown || xx < w->x || xx >= w->x + w->width ||
                    yy < w->y || yy >= w->y + w->height) {
                    continue;
                }
                Slice const *slice = w->slice;
                if (slice == nullptr) {
                    continue;
                }
                auto const *backing = reinterpret_cast<uint32_t const *>(
                    slice->base + w->offset);
                pixel = backing[(yy - w->y) * w->width + (xx - w->x)];
            }
            out[xx] = pixel;
        }
    }
}

/* Composite a rectangle and push it, with the cursor off and on around it. */
void repaint(uint64_t sx, uint64_t sy, uint64_t width, uint64_t height) noexcept
{
    if (sx >= g_width || sy >= g_height) {
        return;
    }
    uint64_t const ex = sx + width > g_width ? g_width : sx + width;
    uint64_t const ey = sy + height > g_height ? g_height : sy + height;
    cursor_erase();
    composite_rect(sx, sy, width, height);
    cursor_draw();
    flush(sx, sy, ex - sx, ey - sy);
}

/* The part of the rectangle at (ax, ay) that the equally-sized rectangle at
 * (kx, ky) does not cover -- for a pure translate that is the x-side band
 * outside the overlap, plus the y-band inside it. */
void composite_uncovered(uint64_t kx, uint64_t ax, uint64_t ay, uint64_t width,
                         uint64_t height, int64_t dx, int64_t dy) noexcept
{
    uint64_t const ox0 = kx > ax ? kx : ax;
    uint64_t const ox1 = kx + width < ax + width ? kx + width : ax + width;
    if (dx > 0) {
        composite_rect(ax + width - static_cast<uint64_t>(dx), ay,
                       static_cast<uint64_t>(dx), height);
    } else if (dx < 0) {
        composite_rect(ax, ay, static_cast<uint64_t>(-dx), height);
    }
    if (ox0 >= ox1) {
        return;
    }
    if (dy > 0) {
        composite_rect(ox0, ay + height - static_cast<uint64_t>(dy), ox1 - ox0,
                       static_cast<uint64_t>(dy));
    } else if (dy < 0) {
        composite_rect(ox0, ay, ox1 - ox0, static_cast<uint64_t>(-dy));
    }
}

/* A window moved: shift the overlap with a memmove and recomposite only the
 * strips the move left or revealed. Compositing the whole union per motion --
 * the window's entire area every time -- is why a drag trailed the cursor:
 * the cursor's 8x8 was cheap, the window's 184k pixels were not. The shift is
 * sound only where the moved window was, and is, the topmost (a drag raises
 * it); otherwise the union is recomposited the general way. */
void repaint_move(Window *window, uint64_t old_x, uint64_t old_y) noexcept
{
    uint64_t const nx = window->x;
    uint64_t const ny = window->y;
    int64_t const dx = static_cast<int64_t>(nx) - static_cast<int64_t>(old_x);
    int64_t const dy = static_cast<int64_t>(ny) - static_cast<int64_t>(old_y);
    if (dx == 0 && dy == 0) {
        return;
    }
    uint64_t const ux = old_x < nx ? old_x : nx;
    uint64_t const uy = old_y < ny ? old_y : ny;
    uint64_t const uright =
        old_x + window->width > nx + window->width ? old_x + window->width
                                                   : nx + window->width;
    uint64_t const ubottom =
        old_y + window->height > ny + window->height ? old_y + window->height
                                                     : ny + window->height;

    if (window->next != nullptr || !window->shown) {
        repaint(ux, uy, uright - ux, ubottom - uy);
        return;
    }

    cursor_erase();
    int64_t const ix0 = static_cast<int64_t>(old_x) > static_cast<int64_t>(nx)
                            ? static_cast<int64_t>(old_x)
                            : static_cast<int64_t>(nx);
    int64_t const iy0 = static_cast<int64_t>(old_y) > static_cast<int64_t>(ny)
                            ? static_cast<int64_t>(old_y)
                            : static_cast<int64_t>(ny);
    int64_t const ix1 =
        static_cast<int64_t>(old_x + window->width) <
                static_cast<int64_t>(nx + window->width)
            ? static_cast<int64_t>(old_x + window->width)
            : static_cast<int64_t>(nx + window->width);
    int64_t const iy1 =
        static_cast<int64_t>(old_y + window->height) <
                static_cast<int64_t>(ny + window->height)
            ? static_cast<int64_t>(old_y + window->height)
            : static_cast<int64_t>(ny + window->height);
    if (ix0 < ix1 && iy0 < iy1) {
        size_t const bytes = static_cast<size_t>(ix1 - ix0) * 4;
        /* Rows ahead of the destination: downward, bottom-up; upward, top-down.
         * memmove carries the horizontal overlap. */
        if (dy > 0) {
            for (int64_t y = iy1 - 1; y >= iy0; --y) {
                uint32_t *dst =
                    reinterpret_cast<uint32_t *>(g_screen + y * g_stride) + ix0;
                uint32_t const *src = reinterpret_cast<uint32_t const *>(
                    g_screen + (y - dy) * g_stride) + (ix0 - dx);
                memmove(dst, src, bytes);
            }
        } else {
            for (int64_t y = iy0; y < iy1; ++y) {
                uint32_t *dst =
                    reinterpret_cast<uint32_t *>(g_screen + y * g_stride) + ix0;
                uint32_t const *src = reinterpret_cast<uint32_t const *>(
                    g_screen + (y - dy) * g_stride) + (ix0 - dx);
                memmove(dst, src, bytes);
            }
        }
    }
    /* The revealed strip (new not old) composites the moved window; the
     * uncovered strip (old not new) composites what is beneath it. */
    composite_uncovered(old_x, nx, ny, window->width, window->height, dx, dy);
    composite_uncovered(nx, old_x, old_y, window->width, window->height, -dx,
                        -dy);
    cursor_draw();
    flush(ux, uy, uright - ux, ubottom - uy);
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    /* What the block names is ours; every slot past it is ours to use (the
     * same adoption every service with an untyped walks). */
    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            if (entry.kind == aegir::bootstrap::EntryKind::Capability &&
                entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            } else if (entry.kind == aegir::bootstrap::EntryKind::DeviceCapability &&
                       entry.reserved + 1 > first_free) {
                first_free = entry.reserved + 1;
            }
        }
    }

    uint64_t untyped_slot = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    bool const given = block != nullptr &&
                       aegir::bootstrap::capability("untyped", 7, &untyped_slot) &&
                       aegir::bootstrap::capability("vspace", 6, &vspace_slot) &&
                       aegir::bootstrap::window(&window_base, &window_bytes);
    if (!given) {
        write_line("FAIL no memory and no address space were given");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    uint64_t untyped_physical = 0;
    uint32_t untyped_bits = 0;
    uint64_t untyped_address = 0;
    static_cast<void>(
        aegir::bootstrap::untyped(&untyped_physical, &untyped_bits, &untyped_address));
    if (!g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                 untyped_physical)) {
        write_line("FAIL the memory I was given would not adopt");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::kCNodeBits) - first_free, 0);
    if (!g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                         static_cast<uintptr_t>(window_base),
                         static_cast<uintptr_t>(window_base + window_bytes), &g_objects)) {
        write_line("FAIL the window I was given could not be adopted");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The devices are reached by name through the registry: the gpu first,
     * its window the screen everything composes into. */
    aegir::ipc::Consumer const registry = aegir::ipc::Consumer::find(
        aegir::registry::kPortName, aegir::registry::kPortNameLength);
    int64_t const gpu_row = registry.valid()
                                ? aegir::registry::find_bound(registry, "gpu.virtio0", 11)
                                : -1;
    seL4_CPtr const gpu_slot = g_objects.alloc_slot();
    uint64_t in[aegir::framebuffer::kInfoWords];
    aegir::ipc::WordsReply info{1, 0};
    aegir::ipc::Consumer gpu(0);
    if (gpu_row >= 0 && gpu_slot != 0 &&
        aegir::registry::open_bound(registry, "gpu.virtio0", 11, gpu_slot)) {
        gpu = aegir::ipc::Consumer(gpu_slot);
        info = gpu.call_words(aegir::framebuffer::kMethodInfo, nullptr, 0, in,
                              aegir::framebuffer::kInfoWords);
    }
    if (!gpu.valid() || info.error != 0 ||
        info.count != aegir::framebuffer::kInfoWords ||
        in[3] != aegir::framebuffer::kFormatB8G8R8X8) {
        write_line("FAIL gpu.virtio0 would not open, or would not say its geometry");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    uint64_t const width = in[0];
    uint64_t const height = in[1];
    uint64_t const stride = in[2];

    /* The window the port serves through is the framebuffer itself: its
     * frames come over one per reply (aegir/registry.h's window and
     * window_frame), and they map here contiguously. */
    uint64_t page_bits = 0;
    uint64_t pages = 0;
    if (!aegir::registry::window_geometry(registry, static_cast<uint64_t>(gpu_row),
                                          &page_bits, &pages) ||
        page_bits != seL4_LargePageBits) {
        write_line("FAIL the gpu's window did not say its shape");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }
    uint8_t *pixels = nullptr;
    for (uint64_t f = 0; f < pages; ++f) {
        seL4_CPtr const frame_slot = g_objects.alloc_slot();
        if (frame_slot == 0 ||
            !aegir::registry::window_frame(registry, static_cast<uint64_t>(gpu_row), f,
                                           frame_slot)) {
            write_line("FAIL a window frame did not come over");
            seL4_Signal(aegir::bootstrap::kSlotSupervision);
            aegir::halt();
        }
        void *const mapped = g_scratch.map_large(frame_slot);
        if (mapped == nullptr) {
            write_line("FAIL a window frame would not map");
            seL4_Signal(aegir::bootstrap::kSlotSupervision);
            aegir::halt();
        }
        if (f == 0) {
            pixels = static_cast<uint8_t *>(mapped);
        } else if (mapped != pixels + (f << page_bits)) {
            write_line("FAIL the window did not map contiguously");
            seL4_Signal(aegir::bootstrap::kSlotSupervision);
            aegir::halt();
        }
    }

    /* The HID devices are the console's, exclusively: opening them here is
     * what makes that true. Their queues feed the routing above; how the
     * console hears about events without holding a next it could not sit
     * in is subscribe (aegir/input.h): one notification, bound to this
     * thread so a signal wakes the same receive that serves the port, and
     * a mint per device badged with the device's own bit. */
    struct {
        char const *name;
        uint32_t length;
    } const hid[] = {{"kbd.virtio0", 11}, {"mouse.virtio0", 13}, {"tablet.virtio0", 14}};
    for (uint32_t d = 0; d < 3; ++d) {
        seL4_CPtr const hid_slot = g_objects.alloc_slot();
        if (hid_slot == 0 ||
            !aegir::registry::open_bound(registry, hid[d].name, hid[d].length, hid_slot)) {
            write_line("FAIL an input device would not open");
            seL4_Signal(aegir::bootstrap::kSlotSupervision);
            aegir::halt();
        }
        g_hid[d] = aegir::ipc::Consumer(hid_slot);
    }
    {
        aegir::mem::Account self{"console", 0, 0, 0};
        seL4_Error wake_error = seL4_NoError;
        seL4_CPtr const wake = g_objects.alloc_object(seL4_NotificationObject,
                                                      seL4_NotificationBits, self,
                                                      &wake_error);
        if (wake == 0 ||
            seL4_TCB_BindNotification(aegir::bootstrap::kSlotOwnTcb, wake) !=
                seL4_NoError) {
            write_line("FAIL the wake notification would not be made or bound");
            seL4_Signal(aegir::bootstrap::kSlotSupervision);
            aegir::halt();
        }
        for (uint32_t d = 0; d < 3; ++d) {
            seL4_CPtr const mint = g_objects.alloc_slot();
            /* A notification's write right is the signal; the badge is the
             * device's bit in the wakeup's word, set high above any service
             * number so the loop can tell a wakeup from a call. */
            if (mint == 0 ||
                seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, mint,
                                aegir::bootstrap::kCNodeBits,
                                aegir::bootstrap::kSlotOwnCNode, wake,
                                aegir::bootstrap::kCNodeBits,
                                seL4_CapRights_new(0, 0, 0, 1),
                                1ull << (32 + d)) != seL4_NoError) {
                write_line("FAIL a device mint would not be made");
                seL4_Signal(aegir::bootstrap::kSlotSupervision);
                aegir::halt();
            }
            bool answered = false;
            uint64_t sink[1];
            aegir::ipc::WordsReply const subscribed = g_hid[d].call_transfer(
                aegir::input::kMethodSubscribe, nullptr, 0, mint, sink, 0, &answered);
            seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, mint,
                              aegir::bootstrap::kCNodeBits);
            if (subscribed.error != 0) {
                write_line("FAIL a device would not take the subscription");
                seL4_Signal(aegir::bootstrap::kSlotSupervision);
                aegir::halt();
            }
        }
    }

    /* The screen the serve loop composites into -- set before the backdrop
     * paint, so the cursor's first draw lands in the first flush. */
    g_gpu = gpu;
    g_screen = pixels;
    g_width = width;
    g_height = height;
    g_stride = stride;
    g_pointer_x = width / 2;
    g_pointer_y = height / 2;

    /* The backdrop, then the cursor over it, then the flush that pushes the
     * window to the screen. */
    for (uint64_t y = 0; y < height; ++y) {
        auto *row = reinterpret_cast<uint32_t *>(pixels + y * stride);
        for (uint64_t x = 0; x < width; ++x) {
            row[x] = kBackdrop;
        }
    }
    cursor_draw();
    flush(0, 0, width, height);
    aegir::debug_write("      console: the backdrop is up -- gpu.virtio0, ");
    aegir::debug_write_unsigned(width);
    aegir::debug_write("x");
    aegir::debug_write_unsigned(height);
    aegir::debug_write(", ");
    aegir::debug_write_unsigned(pages);
    aegir::debug_write(" mega pages mapped\n");

    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Ready));
    }
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    /* The window protocol (aegir/console.h), and the input routing beside
     * it: one receive serves the port, and the bound notification wakes the
     * same receive when a device's queue moved -- a zero-length receive is
     * that wakeup, the badge's bits naming the devices (the mints' badges).
     * The mint slot an attach's `frame` copies into: one, reused, because
     * the reply transfers a copy and ours is deleted right after. */
    uint64_t gui_slot = 0;
    if (!aegir::bootstrap::capability("console.gui", 11, &gui_slot)) {
        write_line("FAIL the console.gui port was not given");
        aegir::halt();
    }
    aegir::mem::Account account{"console", 0, 0, 0};
    aegir::mem::Arena arena(g_objects, g_scratch, account);
    aegir::ipc::Owner gui(static_cast<seL4_CPtr>(gui_slot));
    seL4_CPtr const mint_slot = g_objects.alloc_slot();
    for (;;) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t const info =
            seL4_Recv(static_cast<seL4_CPtr>(gui_slot), &badge);
        uint32_t const length =
            static_cast<uint32_t>(seL4_MessageInfo_get_length(info));
        if ((badge & (7ull << 32)) != 0) {
            /* A wakeup, not a call: the device mints are badged with bits
             * 32..34, above any service number, because a notification that
             * wakes a blocked receive sets the badge register only -- the
             * message registers keep whatever the last call left
             * (notification.c's sendSignal), so the badge is the only thing
             * to read. The test is those three bits exactly, not "any high
             * bit": a user badge carries its class at bit 62
             * (specs/authority.md), and a session's call -- the bureau's --
             * is a call. */
            for (uint32_t d = 0; d < 3; ++d) {
                if ((badge & (1ull << (32 + d))) != 0) {
                    drain(d);
                }
            }
            continue;
        }
        uint32_t const method = static_cast<uint32_t>(seL4_GetMR(0));
        if (method == aegir::console::kMethodAttach && length == 2) {
            /* The slice, carved on demand: a child untyped of the slice's
             * own (reap revokes it), the frames out of it, the pristine
             * mint set, then the console's own mapping -- in that order,
             * because the mints must predate the mapping. One slice per
             * badge: a second attach is the empty reply. */
            uint64_t const bytes = static_cast<uint64_t>(seL4_GetMR(1));
            uint64_t frames = (bytes + (1ull << seL4_LargePageBits) - 1) >>
                              seL4_LargePageBits;
            if (bytes == 0 || find_slice(badge) != nullptr) {
                gui.reply(0);
                continue;
            }
            uint32_t bits = seL4_LargePageBits;
            while ((1ull << bits) < (frames << seL4_LargePageBits)) {
                ++bits;
            }
            seL4_Error carve_error = seL4_NoError;
            uint64_t slice_physical = 0;
            seL4_CPtr const untyped =
                g_objects.carve_untyped(bits, account, &carve_error, &slice_physical);
            /* The slice's home first, room for the two slot sets in it:
             * the carve loop's caps each go to the entry allocated for
             * them, because a cap is named by its own path -- never by one
             * derived from a neighbour's. */
            auto *slice = static_cast<Slice *>(
                untyped != 0
                    ? arena.allocate(sizeof(Slice) +
                                     2 * frames * sizeof(seL4_CPtr))
                    : nullptr);
            uintptr_t base = 0;
            bool carved = slice != nullptr;
            for (uint64_t f = 0; carved && f < frames; ++f) {
                seL4_Error page_error = seL4_NoError;
                seL4_CPtr const frame = g_objects.carve_page(
                    untyped, account, &page_error, seL4_LargePageBits);
                seL4_CPtr const pristine = g_objects.alloc_slot();
                if (frame == 0 || pristine == 0 ||
                    seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, pristine,
                                    aegir::bootstrap::kCNodeBits,
                                    aegir::bootstrap::kSlotOwnCNode, frame,
                                    aegir::bootstrap::kCNodeBits, seL4_AllRights,
                                    0) != seL4_NoError) {
                    carved = false;
                    break;
                }
                slice->slots[f] = frame;
                slice->slots[frames + f] = pristine;
            }
            for (uint64_t f = 0; carved && f < frames; ++f) {
                void *const mapped = g_scratch.map_large(slice->slots[f]);
                if (mapped == nullptr) {
                    carved = false;
                    break;
                }
                if (f == 0) {
                    base = reinterpret_cast<uintptr_t>(mapped);
                }
            }
            /* The client's event channel: one notification, console's to
             * signal, and the ring -- the slice's last page, zeroed here
             * because retyped frames arrive dirty. */
            seL4_Error notify_error = seL4_NoError;
            seL4_CPtr const events =
                carved ? g_objects.alloc_object(seL4_NotificationObject,
                                                seL4_NotificationBits, account,
                                                &notify_error)
                       : 0;
            if (!carved || events == 0) {
                gui.reply(0);
                continue;
            }
            volatile uint64_t *const ring = aegir::console::event_ring(
                reinterpret_cast<uint8_t *>(base), frames << seL4_LargePageBits);
            ring[0] = 0;
            ring[1] = 0;
            *slice = Slice{badge, untyped, frames, base,
                           events, false, g_slices};
            g_slices = slice;
            uint64_t shape[2] = {seL4_LargePageBits, frames};
            gui.reply_words(shape, 2);
        } else if (method == aegir::console::kMethodFrame && length == 2) {
            uint64_t const index = static_cast<uint64_t>(seL4_GetMR(1));
            Slice const *slice = find_slice(badge);
            if (slice == nullptr || index >= slice->frames || mint_slot == 0 ||
                seL4_CNode_Copy(aegir::bootstrap::kSlotOwnCNode, mint_slot,
                                aegir::bootstrap::kCNodeBits,
                                aegir::bootstrap::kSlotOwnCNode,
                                slice->slots[slice->frames + index],
                                aegir::bootstrap::kCNodeBits,
                                seL4_AllRights) != seL4_NoError) {
                gui.reply(0);
            } else {
                gui.reply_cap(nullptr, 0, mint_slot);
                seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, mint_slot,
                                  aegir::bootstrap::kCNodeBits);
            }
        } else if (method == aegir::console::kMethodCreateWindow && length == 7) {
            uint64_t const x = static_cast<uint64_t>(seL4_GetMR(1));
            uint64_t const y = static_cast<uint64_t>(seL4_GetMR(2));
            uint64_t const width = static_cast<uint64_t>(seL4_GetMR(3));
            uint64_t const height = static_cast<uint64_t>(seL4_GetMR(4));
            uint64_t const offset = static_cast<uint64_t>(seL4_GetMR(5));
            uint64_t const flags = static_cast<uint64_t>(seL4_GetMR(6));
            bool const backdrop = (flags & aegir::console::kWindowBackdrop) != 0;
            Slice *slice = find_slice(badge);
            bool const fits =
                slice != nullptr && width != 0 && height != 0 &&
                x + width <= g_width && y + height <= g_height &&
                (offset & 3) == 0 &&
                offset + width * height * 4 <=
                    (slice->frames << seL4_LargePageBits);
            auto *window = static_cast<Window *>(
                fits ? arena.allocate(sizeof(Window)) : nullptr);
            if (window == nullptr) {
                gui.reply(0);
                continue;
            }
            *window = Window{++g_next_id, badge, x, y, width, height, offset,
                             false, backdrop, slice, nullptr};
            /* The list is bottom first: a plain window appends and sits on
             * top; a backdrop enters at the head, beneath everything, and
             * nothing raises it (a button-down focuses, and does not
             * raise). */
            if (backdrop) {
                window->next = g_windows;
                g_windows = window;
            } else {
                Window **tail = &g_windows;
                while (*tail != nullptr) {
                    tail = &(*tail)->next;
                }
                *tail = window;
            }
            if (backdrop) {
                announce_screen_owner(1);
            }
            gui.reply(window->id);
        } else if (method == aegir::console::kMethodDamage && length == 6) {
            uint64_t const id = static_cast<uint64_t>(seL4_GetMR(1));
            uint64_t const rx = static_cast<uint64_t>(seL4_GetMR(2));
            uint64_t const ry = static_cast<uint64_t>(seL4_GetMR(3));
            uint64_t const rw = static_cast<uint64_t>(seL4_GetMR(4));
            uint64_t const rh = static_cast<uint64_t>(seL4_GetMR(5));
            Window *window = find_window(id);
            if (window == nullptr || window->owner != badge) {
                gui.reply(0);
                continue;
            }
            /* The first damage is what shows the window: before it, the
             * backing is the client's to paint and the composite leaves the
             * rectangle to whatever is beneath. */
            window->shown = true;
            /* The rectangle is clipped to the window; a client may damage
             * only its own. */
            uint64_t const cw = rx >= window->width ? 0
                                : rx + rw > window->width
                                    ? window->width - rx
                                    : rw;
            uint64_t const ch = ry >= window->height ? 0
                                : ry + rh > window->height
                                    ? window->height - ry
                                    : rh;
            if (cw != 0 && ch != 0) {
                repaint(window->x + rx, window->y + ry, cw, ch);
            }
            gui.reply(0);
        } else if (method == aegir::console::kMethodListen && length == 1) {
            /* The event channel's cap: a mint of the slice's notification that
             * may wait on it *and* signal it. Read alone would do for the ring,
             * but a client that serves a port hands a signal-only copy of this
             * notification to whoever must ring its doorbell (the bureau,
             * specs/workbench.md), and a mint can only keep rights its source
             * holds. The signal is the client's own doorbell, so waking itself
             * is all the widened right buys. One per client -- a second listen
             * is refused. */
            Slice *slice = find_slice(badge);
            if (slice == nullptr || slice->events == 0 || slice->listening ||
                mint_slot == 0 ||
                seL4_CNode_Mint(aegir::bootstrap::kSlotOwnCNode, mint_slot,
                                aegir::bootstrap::kCNodeBits,
                                aegir::bootstrap::kSlotOwnCNode, slice->events,
                                aegir::bootstrap::kCNodeBits,
                                seL4_CapRights_new(0, 0, 1, 1), 0) != seL4_NoError) {
                gui.reply(0);
            } else {
                slice->listening = true;
                gui.reply_cap(nullptr, 0, mint_slot);
                seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, mint_slot,
                                  aegir::bootstrap::kCNodeBits);
            }
        } else if (method == aegir::console::kMethodDestroyWindow && length == 2) {
            uint64_t const id = static_cast<uint64_t>(seL4_GetMR(1));
            Window **link = &g_windows;
            while (*link != nullptr && (*link)->id != id) {
                link = &(*link)->next;
            }
            if (*link == nullptr || (*link)->owner != badge) {
                gui.reply(0);
                continue;
            }
            Window const gone = **link;
            *link = (*link)->next;
            /* Focus and grab do not outlive the window: the out event is
             * the owner's to hear before the id goes away. */
            if (g_focused != nullptr && g_focused->id == gone.id) {
                deliver(gone.owner, aegir::console::kEventFocus, 0, 0, gone.id);
                g_focused = nullptr;
            }
            if (g_grab != nullptr && g_grab->id == gone.id) {
                g_grab = nullptr;
            }
            /* What was under it is everyone else's redraw. */
            repaint(gone.x, gone.y, gone.width, gone.height);
            if (gone.backdrop) {
                announce_screen_owner(0);
            }
            gui.reply(0);
        } else if (method == aegir::console::kMethodReap && length == 2) {
            /* Session teardown: every window the badge held goes away as if
             * destroyed, then the slice's child untyped is revoked -- the
             * frames, the pristine mints and their copies in the client, and
             * with them the console's own mappings all die in one revoke
             * (finaliseCap unmaps a mapped frame whose cap is deleted), and
             * the memory is the console's to carve again. The console's own
             * allocator never gives slots back, so the dead pristine and
             * notification slots stay spent; a console that reaps daily is
             * the bureau arc's problem. */
            uint64_t const target = static_cast<uint64_t>(seL4_GetMR(1));
            Window **link = &g_windows;
            bool lost_backdrop = false;
            while (*link != nullptr) {
                if ((*link)->owner != target) {
                    link = &(*link)->next;
                    continue;
                }
                Window const gone = **link;
                *link = (*link)->next;
                if (gone.backdrop) {
                    lost_backdrop = true;
                }
                if (g_focused != nullptr && g_focused->id == gone.id) {
                    deliver(gone.owner, aegir::console::kEventFocus, 0, 0,
                            gone.id);
                    g_focused = nullptr;
                }
                if (g_grab != nullptr && g_grab->id == gone.id) {
                    g_grab = nullptr;
                }
                repaint(gone.x, gone.y, gone.width, gone.height);
            }
            Slice **slink = &g_slices;
            while (*slink != nullptr && (*slink)->badge != target) {
                slink = &(*slink)->next;
            }
            if (*slink != nullptr) {
                Slice *const dead = *slink;
                *slink = dead->next;
                seL4_CNode_Revoke(aegir::bootstrap::kSlotOwnCNode, dead->untyped,
                                  aegir::bootstrap::kCNodeBits);
                seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                                  dead->untyped, aegir::bootstrap::kCNodeBits);
                seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                                  dead->events, aegir::bootstrap::kCNodeBits);
            }
            if (lost_backdrop) {
                announce_screen_owner(0);
            }
            gui.reply(0);
        } else if (method == aegir::console::kMethodMove && length == 4) {
            uint64_t const id = static_cast<uint64_t>(seL4_GetMR(1));
            uint64_t const x = static_cast<uint64_t>(seL4_GetMR(2));
            uint64_t const y = static_cast<uint64_t>(seL4_GetMR(3));
            Window *const window = find_window(id);
            if (window == nullptr || window->owner != badge ||
                x + window->width > g_width || y + window->height > g_height) {
                gui.reply(0);
                continue;
            }
            /* Move the window and recomposite: the overlap is shifted and only the
             * strips are redrawn (repaint_move, specs/window-manager.md). */
            uint64_t const old_x = window->x;
            uint64_t const old_y = window->y;
            window->x = x;
            window->y = y;
            repaint_move(window, old_x, old_y);
            gui.reply(0);
        } else if (method == aegir::console::kMethodRaise && length == 2) {
            uint64_t const id = static_cast<uint64_t>(seL4_GetMR(1));
            Window **link = &g_windows;
            while (*link != nullptr && (*link)->id != id) {
                link = &(*link)->next;
            }
            Window *const window = *link;
            /* A backdrop stays at the bottom: a raise of one is refused. */
            if (window == nullptr || window->owner != badge || window->backdrop) {
                gui.reply(0);
                continue;
            }
            *link = window->next;
            Window **tail = &g_windows;
            while (*tail != nullptr) {
                tail = &(*tail)->next;
            }
            window->next = nullptr;
            *tail = window;
            repaint(window->x, window->y, window->width, window->height);
            gui.reply(0);
        } else if (method == aegir::console::kMethodResize && length == 4) {
            uint64_t const id = static_cast<uint64_t>(seL4_GetMR(1));
            uint64_t const width = static_cast<uint64_t>(seL4_GetMR(2));
            uint64_t const height = static_cast<uint64_t>(seL4_GetMR(3));
            Window *const window = find_window(id);
            Slice const *slice =
                window != nullptr ? find_slice(window->owner) : nullptr;
            bool const fits =
                window != nullptr && window->owner == badge && width != 0 &&
                height != 0 && window->x + width <= g_width &&
                window->y + height <= g_height &&
                window->offset + width * height * 4 <=
                    (slice != nullptr ? slice->frames << seL4_LargePageBits : 0);
            if (!fits) {
                gui.reply(0);
                continue;
            }
            /* The origin does not move, so the union of the old and the new
             * rectangle is just the larger of the two: the area the window
             * grew into, or the strip it left if it shrank. */
            uint64_t const old_width = window->width;
            uint64_t const old_height = window->height;
            window->width = width;
            window->height = height;
            uint64_t const widest = old_width > width ? old_width : width;
            uint64_t const tallest = old_height > height ? old_height : height;
            repaint(window->x, window->y, widest, tallest);
            gui.reply(0);
        } else if (method == aegir::console::kMethodLower && length == 2) {
            uint64_t const id = static_cast<uint64_t>(seL4_GetMR(1));
            Window **link = &g_windows;
            while (*link != nullptr && (*link)->id != id) {
                link = &(*link)->next;
            }
            Window *const window = *link;
            if (window == nullptr || window->owner != badge || window->backdrop) {
                gui.reply(0);
                continue;
            }
            /* To the bottom among the plain windows: unlink, then insert
             * past the last backdrop -- the head of the list is the bottom. */
            *link = window->next;
            Window **after = &g_windows;
            while (*after != nullptr && (*after)->backdrop) {
                after = &(*after)->next;
            }
            window->next = *after;
            *after = window;
            repaint(window->x, window->y, window->width, window->height);
            gui.reply(0);
        } else if (method == aegir::console::kMethodFocus && length == 2) {
            uint64_t const id = static_cast<uint64_t>(seL4_GetMR(1));
            Window *const window = find_window(id);
            /* A backdrop is the screen, not a thing to focus; and a window
             * already focused has nothing to change. No raise: focus and
             * depth stay separate (specs/console.md). */
            if (window == nullptr || window->owner != badge || window->backdrop ||
                g_focused == window) {
                gui.reply(0);
                continue;
            }
            if (g_focused != nullptr) {
                deliver(g_focused->owner, aegir::console::kEventFocus, 0, 0,
                        g_focused->id);
            }
            g_focused = window;
            deliver(window->owner, aegir::console::kEventFocus, 0, 1, window->id);
            gui.reply(0);
        } else if (method == aegir::console::kMethodInfo && length == 1) {
            /* The screen's size, whatever mode the driver settled on: the
             * bureau sizes its backdrop from it (specs/bureau.md). */
            uint64_t const size[2] = {g_width, g_height};
            gui.reply_words(size, 2);
        } else {
            /* A method we do not know: the answer says so by saying nothing
             * (aegir/console.h). */
            gui.reply(0);
        }
    }
}
