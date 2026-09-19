/*
 * aegir-bureau: the session a greeter login starts -- the screen, handed over.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The bureau is the Amiga screen as a shape of window (specs/console.md's
 * login arc): one full-screen window, always in backdrop mode -- the
 * Workbench grey, flat, no gadgets -- which is what every Amiga user turned
 * on anyway. It is deliberately a stub: it attaches its slice of the
 * console's arena, paints, damages, says so, and exits. The window persists
 * because the console owns the slice; the process's memory reclaims through
 * the session's ordinary path, and what a bureau grows into (icons, windows
 * of one's own, the re-login that reaps this backdrop) is the bureau arc's,
 * not this stub's.
 *
 * The screen's size is a constant here: the console never serves it, and a
 * stub that asks for the wrong size is refused loudly at create. A size
 * query is the window protocol's next method when a second client needs it.
 */

#include <aegir/bootstrap.h>
#include <aegir/console.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <sel4/sel4.h>
#include <stdint.h>

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

/* The screen the bureau covers, and its grey -- Workbench grey, in the
 * driver's B8G8R8X8 (0x00RRGGBB). */
constexpr uint64_t kScreenWidth = 1280;
constexpr uint64_t kScreenHeight = 800;
constexpr uint32_t kGrey = 0x00AAAAAA;
constexpr uint64_t kSliceBytes = 4ull << 20; /* two mega pages hold the backing */

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

    aegir::ipc::Consumer const gui = aegir::ipc::Consumer::find(
        aegir::console::kPortName, aegir::console::kPortNameLength);
    if (!gui.valid()) {
        write("  bureau: FAIL no console.gui\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The mapping authority: the delegated untyped (page tables are retyped
     * from it), the VSpace root, and the window of free addresses the block
     * names -- the session spawn's give_vspace grant, the greeter's shape. */
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
        write("  bureau: FAIL no untyped, vspace or window\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* The slice: two mega pages -- the backing is 1280x800x4 bytes at offset
     * 0, and the event ring's page at the end stays untouched (a backdrop
     * listens for nothing). The two frames must land contiguously: the
     * backing is one range. */
    seL4_CPtr const frame0 = g_objects.alloc_slot();
    seL4_CPtr const frame1 = g_objects.alloc_slot();
    uint64_t frame_bits = 0;
    uint64_t frames = 0;
    ok = frame0 != 0 && frame1 != 0 &&
         aegir::console::attach(gui, kSliceBytes, &frame_bits, &frames) &&
         frame_bits == seL4_LargePageBits && frames == 2 &&
         aegir::console::frame(gui, 0, frame0) &&
         aegir::console::frame(gui, 1, frame1);
    uint8_t *const slice =
        static_cast<uint8_t *>(ok ? g_scratch.map_large(frame0) : nullptr);
    uint8_t *const upper =
        static_cast<uint8_t *>(ok && slice != nullptr ? g_scratch.map_large(frame1)
                                                      : nullptr);
    ok = ok && slice != nullptr && upper == slice + (1ull << seL4_LargePageBits);
    uint64_t const window =
        ok ? aegir::console::create_window(gui, 0, 0, kScreenWidth, kScreenHeight,
                                           0, aegir::console::kWindowBackdrop)
           : 0;
    ok = ok && window != 0;
    if (!ok) {
        write("  bureau: FAIL the console's channel would not open\n");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
        aegir::halt();
    }

    /* Backdrop mode: one grey, edge to edge. */
    auto *pixels = reinterpret_cast<uint32_t *>(slice);
    for (uint64_t p = 0; p < kScreenWidth * kScreenHeight; ++p) {
        pixels[p] = kGrey;
    }
    (void)aegir::console::damage(gui, window, 0, 0, kScreenWidth, kScreenHeight);

    write("  bureau: the screen is yours\n");
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Ready));
    }
    /* The exit is the handoff: the window stays (the console owns the
     * slice), the session's reclaim takes the rest, and auth goes back to
     * serving. */
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
