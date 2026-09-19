/*
 * aegir-greeter: the login window.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Auth starts one of these once the user database is read (specs/console.md's
 * login arc): auth's face, a system child badged from auth's own range
 * (specs/authority.md), asking on the screen what the serial line asked
 * before it. It attaches a slice of the console's arena, draws the form into
 * its window -- a name field, a secret field that echoes bullets, a button --
 * and edits them off the event channel: keys land window-local and translated
 * (the keymap is the console's), a click focuses what it lands in, Tab moves
 * between fields, Enter asks auth.login.
 *
 * A refused answer clears the secret and says so in the form. An accepted one
 * is this process's exit: auth started the session the login earned, and the
 * window and slice go back with auth's reap of this badge -- the greeter
 * draws nothing after the reply, so the teardown's revoke never meets a live
 * mapping in here.
 */

#include <aegir/authdb.h>
#include <aegir/bootstrap.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/input.h>
#include <aegir/ipc/port.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/nmspace.h>
#include <sel4/sel4.h>
#include <stdint.h>

#include "font.h"

namespace {

/* Static, both of them: the allocator's untyped table and the scratch
 * window's bookkeeping live in the object, and a stack copy would die with
 * the frame that made it. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

void write(char const *text)
{
    aegir::debug_write(text);
}

void write(char const *text, uint32_t length)
{
    aegir::debug_write(text, length);
}

/* The form's geometry, window-local. Two fields under their labels, the
 * button below, the refusal line at the bottom. */
constexpr uint64_t kWindowX = 400;
constexpr uint64_t kWindowY = 220;
constexpr uint64_t kWindowWidth = 480;
constexpr uint64_t kWindowHeight = 360;
constexpr uint64_t kSliceBytes = 2ull << 20;

constexpr uint64_t kFieldX = 24;
constexpr uint64_t kFieldWidth = 432;
constexpr uint64_t kFieldHeight = 24;
constexpr uint64_t kNameLabelY = 40;
constexpr uint64_t kNameFieldY = 56;
constexpr uint64_t kSecretLabelY = 104;
constexpr uint64_t kSecretFieldY = 120;
constexpr uint64_t kButtonX = 24;
constexpr uint64_t kButtonY = 168;
constexpr uint64_t kButtonWidth = 120;
constexpr uint64_t kButtonHeight = 32;
constexpr uint64_t kErrorY = 224;

/* The palette, one word a pixel in the driver's B8G8R8X8 (0x00RRGGBB). */
constexpr uint32_t kGrey = 0x00A0A0A0;
constexpr uint32_t kWhite = 0x00FFFFFF;
constexpr uint32_t kBlack = 0x00000000;
constexpr uint32_t kBlue = 0x000000FF;
constexpr uint32_t kDark = 0x00505050;
constexpr uint32_t kRed = 0x00FF0000;

/* The form's state: what has been typed, where typing goes, and whether the
 * last ask was refused. The widths are the database row's (aegir/authdb.h) --
 * a field holds one less than its column, so the wire's pack always fits. */
struct Form {
    char name[aegir::authdb::kNameBytes];
    uint32_t name_length;
    char secret[aegir::authdb::kSecretBytes];
    uint32_t secret_length;
    uint32_t focused; /* 0 the name, 1 the secret */
    bool refused;
};

void fill_rect(uint32_t *pixels, uint64_t x, uint64_t y, uint64_t width,
               uint64_t height, uint32_t colour) noexcept
{
    for (uint64_t row = 0; row < height; ++row) {
        for (uint64_t col = 0; col < width; ++col) {
            pixels[(y + row) * kWindowWidth + x + col] = colour;
        }
    }
}

void draw_field(uint32_t *pixels, uint64_t y, bool focused) noexcept
{
    fill_rect(pixels, kFieldX - 2, y - 2, kFieldWidth + 4, kFieldHeight + 4,
              focused ? kBlue : kDark);
    fill_rect(pixels, kFieldX, y, kFieldWidth, kFieldHeight, kWhite);
}

/* The whole form, redrawn on every change: 480x360 is 172800 words, and the
 * keystroke that caused it costs more than the fill. */
void draw(uint32_t *pixels, Form const &form) noexcept
{
    fill_rect(pixels, 0, 0, kWindowWidth, kWindowHeight, kGrey);
    greeter::draw_text(pixels, kWindowWidth, kFieldX, kNameLabelY, "name:", 5,
                       kBlack);
    greeter::draw_text(pixels, kWindowWidth, kFieldX, kSecretLabelY, "secret:", 7,
                       kBlack);
    draw_field(pixels, kNameFieldY, form.focused == 0);
    draw_field(pixels, kSecretFieldY, form.focused == 1);
    greeter::draw_text(pixels, kWindowWidth, kFieldX + 8, kNameFieldY + 8,
                       form.name, form.name_length, kBlack);
    /* The secret echoes as bullets: what is typed is the credential's, not
     * the screen's. */
    for (uint32_t i = 0; i < form.secret_length; ++i) {
        greeter::draw_char(pixels, kWindowWidth, kFieldX + 8 + i * 8,
                           kSecretFieldY + 8, '*', kBlack);
    }
    fill_rect(pixels, kButtonX, kButtonY, kButtonWidth, kButtonHeight, kDark);
    greeter::draw_text(pixels, kWindowWidth, kButtonX + 28, kButtonY + 12,
                       "log in", 6, kWhite);
    if (form.refused) {
        greeter::draw_text(pixels, kWindowWidth, kFieldX, kErrorY,
                           "no such name or secret", 22, kRed);
    }
}

/* The login ask (aegir/authdb.h's wire): the name's words, then the secret's,
 * one word back -- 1 authenticated, 0 refused. */
uint64_t login(aegir::ipc::Consumer const &port, Form const &form) noexcept
{
    uint64_t out[aegir::ipc::kMaxWords];
    uint32_t words = aegir::nmspace::pack_string(out, form.name, form.name_length,
                                                 aegir::authdb::kNameBytes);
    if (words == 0) {
        return ~0ULL;
    }
    uint32_t const secret_words =
        aegir::nmspace::pack_string(out + words, form.secret, form.secret_length,
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

/* Is (x, y) inside the rectangle? Window-local, as the events arrive. */
bool inside(uint64_t x, uint64_t y, uint64_t rx, uint64_t ry, uint64_t width,
            uint64_t height) noexcept
{
    return x >= rx && x < rx + width && y >= ry && y < ry + height;
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

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

    /* The mapping authority: the delegated untyped (page tables are retyped
     * from it), the VSpace root, and the window of free addresses the block
     * names -- the give_vspace grant (aegir/spawn/process.h). */
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
    if (!ok) {
        write("  greeter: FAIL no untyped, vspace or window\n");
        aegir::halt();
    }

    /* The slice and the channel: one mega page holds the window's backing
     * (480x360x4 = 691200 bytes at offset 0) and the event ring (its last
     * page). One frame call, one listen, one mapping. */
    seL4_CPtr const slice_frame = g_objects.alloc_slot();
    seL4_CPtr const events = g_objects.alloc_slot();
    uint64_t frame_bits = 0;
    uint64_t frames = 0;
    ok = slice_frame != 0 && events != 0 &&
         aegir::console::attach(gui, kSliceBytes, &frame_bits, &frames) &&
         frame_bits == seL4_LargePageBits && frames == 1 &&
         aegir::console::frame(gui, 0, slice_frame) &&
         aegir::console::listen(gui, events);
    uint8_t *const slice =
        static_cast<uint8_t *>(ok ? g_scratch.map_large(slice_frame) : nullptr);
    ok = ok && slice != nullptr;
    uint64_t const window =
        ok ? aegir::console::create_window(gui, kWindowX, kWindowY, kWindowWidth,
                                           kWindowHeight, 0)
           : 0;
    ok = ok && window != 0;
    if (!ok) {
        write("  greeter: FAIL the console's channel would not open\n");
        aegir::halt();
    }
    volatile uint64_t *const ring = aegir::console::event_ring(slice, kSliceBytes);
    uint32_t *const pixels = reinterpret_cast<uint32_t *>(slice);

    greeter::font_parse();
    Form form{{}, 0, {}, 0, 0, false};
    draw(pixels, form);
    (void)aegir::console::damage(gui, window, 0, 0, kWindowWidth, kWindowHeight);
    write("  greeter: a name and a secret, please\n");
    /* The supervision signal is the spawner's clock: this one says the form
     * is on the screen (auth waits for it before the boot moves on, so the
     * cue never shares the serial with another service's line), and the one
     * on the accept path below says this process is done. */
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    /* The edit loop: drain the ring, redraw what changed, wait for more.
     * Keys arrive translated (the console's keymap, US v1): '\t' moves
     * between the fields, '\n' asks, '\b' takes a character back, and a
     * printable appends where the focus is. A click focuses what it lands
     * in -- the button asks, a field takes the focus. */
    bool accepted = false;
    bool focus_announced = false;
    while (!accepted) {
        uint64_t event = 0;
        uint64_t event_window = 0;
        if (!aegir::console::ring_take(ring, &event, &event_window)) {
            seL4_Wait(events, nullptr);
            continue;
        }
        uint16_t const type = aegir::input::event_type(event);
        uint32_t const value = aegir::input::event_value(event);
        bool changed = false;
        /* The focus is announced once, and it is the runner's cue to type:
         * the click that lands it rides the mouse's queue while the keys
         * ride the keyboard's, and which queue the console drains first is
         * the boot's timing -- so the keys answer this line, when the
         * routing is already the greeter's (scripts/targets.py). */
        if (type == aegir::console::kEventFocus && value == 1 &&
            !focus_announced) {
            focus_announced = true;
            write("  greeter: the window has the focus\n");
        }
        bool submit = false;
        if (type == aegir::console::kEventKey &&
            (value & aegir::console::kKeyPressed) != 0) {
            char const c = static_cast<char>(value & 0xffff);
            if (c == '\t') {
                form.focused ^= 1;
                changed = true;
            } else if (c == '\n') {
                submit = true;
            } else if (c == '\b') {
                if (form.focused == 0 && form.name_length > 0) {
                    --form.name_length;
                    changed = true;
                } else if (form.focused == 1 && form.secret_length > 0) {
                    --form.secret_length;
                    changed = true;
                }
            } else if (c >= 32 && c < 127) {
                if (form.focused == 0 &&
                    form.name_length + 1 < aegir::authdb::kNameBytes) {
                    form.name[form.name_length++] = c;
                    changed = true;
                } else if (form.focused == 1 &&
                           form.secret_length + 1 < aegir::authdb::kSecretBytes) {
                    form.secret[form.secret_length++] = c;
                    changed = true;
                }
            }
        } else if (type == aegir::console::kEventPointer &&
                   aegir::input::event_code(event) == aegir::input::kBtnLeft) {
            uint64_t const x = value & 0xffff;
            uint64_t const y = (value >> 16) & 0xffff;
            if (inside(x, y, kButtonX, kButtonY, kButtonWidth, kButtonHeight)) {
                submit = true;
            } else if (inside(x, y, kFieldX, kNameFieldY, kFieldWidth,
                              kFieldHeight)) {
                changed = form.focused != 0;
                form.focused = 0;
            } else if (inside(x, y, kFieldX, kSecretFieldY, kFieldWidth,
                              kFieldHeight)) {
                changed = form.focused != 1;
                form.focused = 1;
            }
        }
        if (submit) {
            uint64_t const answer = login(auth_login, form);
            if (answer == 1) {
                accepted = true;
            } else {
                /* Refused -- or the ask itself broke, which reads the same
                 * to the person at the screen. The secret does not survive
                 * a refusal. */
                form.refused = true;
                form.secret_length = 0;
                changed = true;
            }
        } else if (changed && form.refused) {
            /* Typing past a refusal clears it: the line is about the last
             * ask, and the next keystroke is the next ask beginning. */
            form.refused = false;
        }
        if (changed && !accepted) {
            draw(pixels, form);
            (void)aegir::console::damage(gui, window, 0, 0, kWindowWidth,
                                         kWindowHeight);
        }
    }

    /* Accepted: auth has the session going and reaps the window and slice
     * with this badge -- nothing below touches the slice, because the
     * teardown's revoke would meet any live mapping in here with a fault. */
    write("  greeter: welcome, ");
    write(form.name, form.name_length);
    write(" -- the bureau takes it from here\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
