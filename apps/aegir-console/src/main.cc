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
#include <aegir/debug.h>
#include <aegir/framebuffer.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/registry.h>
#include <sel4/sel4.h>
#include <stdint.h>

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

/* Static, not local, and that is not a style choice: an Allocator carries
 * the tables of what it handed out, and a service's stack is pages, not
 * tables (the device manager says the same of its own). */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);

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
    g_objects.adopt_slots(first_free, (1u << 10) - first_free, 0);
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
     * what makes that true. Routing their events is the input piece's; the
     * ports are held, not yet read. */
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
    }

    /* The backdrop, then the flush that pushes the window to the screen. */
    for (uint64_t y = 0; y < height; ++y) {
        auto *row = reinterpret_cast<uint32_t *>(pixels + y * stride);
        for (uint64_t x = 0; x < width; ++x) {
            row[x] = kBackdrop;
        }
    }
    (void)gpu.call(aegir::framebuffer::kMethodFlush, 0);
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

    /* The port answers already, and knows nothing yet: every method is the
     * empty reply, the version rule (aegir/registry.h's shape). */
    uint64_t gui_slot = 0;
    if (!aegir::bootstrap::capability("console.gui", 11, &gui_slot)) {
        write_line("FAIL the console.gui port was not given");
        aegir::halt();
    }
    aegir::ipc::Owner gui(static_cast<seL4_CPtr>(gui_slot));
    for (;;) {
        seL4_Word badge = 0;
        seL4_Recv(static_cast<seL4_CPtr>(gui_slot), &badge);
        if ((badge & aegir::ipc::kCallMark) == 0) {
            continue; /* a signal, not a call: nobody's to answer */
        }
        gui.reply(0);
    }
}
