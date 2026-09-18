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

/* The screen the window protocol composites into: the gpu port, its
 * mapped window, and the geometry `info` answered. */
aegir::ipc::Consumer g_gpu(0);
uint8_t *g_screen = nullptr;
uint64_t g_width = 0;
uint64_t g_height = 0;
uint64_t g_stride = 0;

/* A client's slice of the arena: megapage frames retyped from a child
 * untyped of the slice's own, so a reap revokes exactly one client's
 * pixels (authority.md's retained-copy path). The console maps the carved
 * set -- it composites through it -- and hands copies out of the pristine
 * mint set, made before any mapping, because a mapped cap's copies are
 * pinned to its ASID and useless to another address space
 * (kernel/src/arch/riscv/kernel/vspace.c:869-878). */
struct Slice {
    uint64_t badge;
    seL4_CPtr untyped;  /* the slice's own: revoking it reclaims the whole */
    seL4_CPtr pristine; /* base of the unmapped copy set, one slot per frame */
    uint64_t frames;
    uintptr_t base; /* where the console reads the slice */
    Slice *next;
};

/* A window: a rectangle on the screen and where its pixels live in the
 * owner's slice. The list is in z order, bottom first; create appends, so
 * new windows sit on top. */
struct Window {
    uint64_t id;
    uint64_t owner; /* the badge create_window arrived with */
    uint64_t x;
    uint64_t y;
    uint64_t width;
    uint64_t height;
    uint64_t offset; /* the backing's offset within the owner's slice */
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

/* Composite a screen rectangle: every pixel is the topmost window covering
 * it, or the backdrop. The windows are walked bottom to top, so the last
 * coverer wins. */
void repaint(uint64_t sx, uint64_t sy, uint64_t width, uint64_t height) noexcept
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
                if (xx < w->x || xx >= w->x + w->width || yy < w->y ||
                    yy >= w->y + w->height) {
                    continue;
                }
                Slice const *slice = find_slice(w->owner);
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
    (void)g_gpu.call(aegir::framebuffer::kMethodFlush, 0);
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

    /* The screen the serve loop composites into. */
    g_gpu = gpu;
    g_screen = pixels;
    g_width = width;
    g_height = height;
    g_stride = stride;

    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Ready));
    }
    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    /* The window protocol (aegir/console.h). One endpoint, and every
     * receive on it is a call -- endpoints do not signal -- so there is no
     * badge mark to tell apart here; the caller's badge is who the slice
     * and the windows belong to. The mint slot an attach's `frame` copies
     * into: one, reused, because the reply transfers a copy and ours is
     * deleted right after. */
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
            seL4_CPtr carved_base = 0;
            seL4_CPtr pristine_base = 0;
            uintptr_t base = 0;
            bool carved = untyped != 0;
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
                if (f == 0) {
                    carved_base = frame;
                    pristine_base = pristine;
                }
            }
            for (uint64_t f = 0; carved && f < frames; ++f) {
                void *const mapped = g_scratch.map_large(carved_base +
                                                         static_cast<seL4_CPtr>(f));
                if (mapped == nullptr) {
                    carved = false;
                    break;
                }
                if (f == 0) {
                    base = reinterpret_cast<uintptr_t>(mapped);
                }
            }
            auto *slice = static_cast<Slice *>(
                carved ? arena.allocate(sizeof(Slice)) : nullptr);
            if (slice == nullptr) {
                gui.reply(0);
                continue;
            }
            *slice = Slice{badge, untyped, pristine_base, frames, base, g_slices};
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
                                slice->pristine + static_cast<seL4_CPtr>(index),
                                aegir::bootstrap::kCNodeBits,
                                seL4_AllRights) != seL4_NoError) {
                gui.reply(0);
            } else {
                gui.reply_cap(nullptr, 0, mint_slot);
                seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, mint_slot,
                                  aegir::bootstrap::kCNodeBits);
            }
        } else if (method == aegir::console::kMethodCreateWindow && length == 6) {
            uint64_t const x = static_cast<uint64_t>(seL4_GetMR(1));
            uint64_t const y = static_cast<uint64_t>(seL4_GetMR(2));
            uint64_t const width = static_cast<uint64_t>(seL4_GetMR(3));
            uint64_t const height = static_cast<uint64_t>(seL4_GetMR(4));
            uint64_t const offset = static_cast<uint64_t>(seL4_GetMR(5));
            Slice const *slice = find_slice(badge);
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
                             nullptr};
            /* Append: the list is bottom first, and a new window is on top. */
            Window **tail = &g_windows;
            while (*tail != nullptr) {
                tail = &(*tail)->next;
            }
            *tail = window;
            gui.reply(window->id);
        } else if (method == aegir::console::kMethodDamage && length == 6) {
            uint64_t const id = static_cast<uint64_t>(seL4_GetMR(1));
            uint64_t const rx = static_cast<uint64_t>(seL4_GetMR(2));
            uint64_t const ry = static_cast<uint64_t>(seL4_GetMR(3));
            uint64_t const rw = static_cast<uint64_t>(seL4_GetMR(4));
            uint64_t const rh = static_cast<uint64_t>(seL4_GetMR(5));
            Window const *window = find_window(id);
            if (window == nullptr || window->owner != badge) {
                gui.reply(0);
                continue;
            }
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
            /* What was under it is everyone else's redraw. */
            repaint(gone.x, gone.y, gone.width, gone.height);
            gui.reply(0);
        } else {
            /* A method we do not know: the answer says so by saying nothing
             * (aegir/console.h). */
            gui.reply(0);
        }
    }
}
