/*
 * aegir-virtio-gpu: the driver for the display.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * virtio device 16, bound from the registry row that names this binary -- the
 * fourth virtio driver, the first output one, and the first bound twice: two
 * transports on QEMU's command line are two bindings of one row, and the
 * per-binding spawn loop starts one of these per head (specs/services.md).
 *
 * The framebuffer is the shared window: ATTACH_BACKING points the device at
 * the window's physical base, so what the port's clients write is what the
 * screen shows, and `flush` is the push (aegir/framebuffer.h). The window
 * rides as mega pages -- 32 MiB of it, because 3840x2160 at 32 bits a pixel
 * fits in 32 MiB -- which is what window_page_bits in the spawn request is
 * for.
 */

#include "gpu.h"

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/framebuffer.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <aegir/virtio/handshake.h>
#include <aegir/virtio/mmio.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

void write_line(char const *label, char const *text) noexcept
{
    aegir::debug_write("      ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    aegir::debug_write(text);
    aegir::debug_write("\n");
}

/* The screen as this driver knows it: what the device said the display is,
 * what it said the glass measures, and the resource the scanout points at. */
uint32_t g_width = 0;
uint32_t g_height = 0;
uint32_t g_phys_width_mm = 0;
uint32_t g_phys_height_mm = 0;
uint32_t g_resource = 0;
uint32_t g_next_resource = 1;

uint64_t g_window_address = 0;
uint32_t g_window_bytes = 0;
uint64_t g_window_physical = 0;

/* The self-test content, and what a mode change leaves on the screen until a
 * client owns it: three vertical bands, red, green, blue. The pixel word is
 * 0x00RRGGBB, because B8G8R8X8 is bytes B, G, R, X in memory. */
void paint_bands() noexcept
{
    static constexpr uint32_t kBands[] = {0x00ff0000u, 0x0000ff00u, 0x000000ffu};
    volatile uint32_t *pixels = reinterpret_cast<volatile uint32_t *>(g_window_address);
    for (uint32_t y = 0; y < g_height; ++y) {
        for (uint32_t x = 0; x < g_width; ++x) {
            pixels[y * g_width + x] = kBands[(x * 3) / g_width];
        }
    }
}

/* The frame's trip to the screen: the given region of the window becomes the
 * resource's contents, and the flush is what the display shows. The region is
 * what changed -- a keystroke or a drag pushes its rectangle, not the whole
 * screen; copying all 4 MiB per event was the console's lag. */
bool present(aegir::virtio::Registers const &registers, aegir::virtio::Queue &queue,
             uint32_t x, uint32_t y, uint32_t width, uint32_t height) noexcept
{
    if (width == 0 || height == 0) {
        return true;
    }
    aegir::virtio::TransferToHost2d transfer{};
    transfer.hdr.type = aegir::virtio::kGpuCmdTransferToHost2d;
    transfer.r.x = x;
    transfer.r.y = y;
    transfer.r.width = width;
    transfer.r.height = height;
    transfer.offset = static_cast<uint64_t>(y) * g_width * 4 +
                      static_cast<uint64_t>(x) * 4;
    transfer.resource_id = g_resource;
    aegir::virtio::CtrlHeader answer{};
    aegir::virtio::GpuResult result =
        aegir::virtio::control(registers, queue, &transfer, sizeof(transfer), &answer,
                               sizeof(answer));
    if (!result.completed || result.response_type != aegir::virtio::kGpuRespOkNodata) {
        return false;
    }

    aegir::virtio::Flush flush{};
    flush.hdr.type = aegir::virtio::kGpuCmdResourceFlush;
    flush.r.x = x;
    flush.r.y = y;
    flush.r.width = width;
    flush.r.height = height;
    flush.resource_id = g_resource;
    result = aegir::virtio::control(registers, queue, &flush, sizeof(flush), &answer,
                                    sizeof(answer));
    return result.completed && result.response_type == aegir::virtio::kGpuRespOkNodata;
}

/* Point the scanout at a new resource of the given size, backed by the
 * window from its base. The old resource is unrefed once the scanout no
 * longer points at it. False is the command that failed, said by the
 * caller. */
bool apply_mode(aegir::virtio::Registers const &registers, aegir::virtio::Queue &queue,
                uint32_t width, uint32_t height) noexcept
{
    uint32_t const fresh = ++g_next_resource;

    aegir::virtio::Create2d create{};
    create.hdr.type = aegir::virtio::kGpuCmdResourceCreate2d;
    create.resource_id = fresh;
    create.format = aegir::virtio::kGpuFormatB8G8R8X8;
    create.width = width;
    create.height = height;
    aegir::virtio::CtrlHeader answer{};
    aegir::virtio::GpuResult result =
        aegir::virtio::control(registers, queue, &create, sizeof(create), &answer,
                               sizeof(answer));
    if (!result.completed || result.response_type != aegir::virtio::kGpuRespOkNodata) {
        return false;
    }

    aegir::virtio::AttachBacking attach{};
    attach.hdr.type = aegir::virtio::kGpuCmdResourceAttachBacking;
    attach.resource_id = fresh;
    attach.nr_entries = 1;
    attach.addr = g_window_physical;
    attach.length = width * height * 4;
    result = aegir::virtio::control(registers, queue, &attach, sizeof(attach), &answer,
                                    sizeof(answer));
    if (!result.completed || result.response_type != aegir::virtio::kGpuRespOkNodata) {
        return false;
    }

    aegir::virtio::SetScanout scanout{};
    scanout.hdr.type = aegir::virtio::kGpuCmdSetScanout;
    scanout.r.width = width;
    scanout.r.height = height;
    scanout.scanout_id = 0;
    scanout.resource_id = fresh;
    result = aegir::virtio::control(registers, queue, &scanout, sizeof(scanout), &answer,
                                    sizeof(answer));
    if (!result.completed || result.response_type != aegir::virtio::kGpuRespOkNodata) {
        return false;
    }

    if (g_resource != 0) {
        aegir::virtio::Unref unref{};
        unref.hdr.type = aegir::virtio::kGpuCmdResourceUnref;
        unref.resource_id = g_resource;
        (void)aegir::virtio::control(registers, queue, &unref, sizeof(unref), &answer,
                                     sizeof(answer));
    }
    g_resource = fresh;
    g_width = width;
    g_height = height;
    return true;
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
    } else {
        write_line("log.main port", "not given");
    }

    write_line("virtio-gpu", "looking for my device");

    uint64_t device_address = 0;
    uint32_t device_bytes = 0;
    uint64_t device_physical = 0;
    if (!aegir::bootstrap::device(&device_address, &device_bytes, &device_physical)) {
        write_line("FAIL", "no device was given to me");
        return 0;
    }
    aegir::virtio::Registers const registers(device_address);

    uint32_t const magic = registers.read(aegir::virtio::kMagicValue);
    uint32_t const device_id = registers.read(aegir::virtio::kDeviceId);
    if (magic != aegir::virtio::kMagic || device_id != aegir::virtio::kDeviceIdGpu) {
        write_line("FAIL", "my device is not the gpu");
        return 0;
    }

    uint64_t memory_physical = 0;
    uint32_t memory_bits = 0;
    uint64_t memory_address = 0;
    if (!aegir::bootstrap::untyped(&memory_physical, &memory_bits, &memory_address)) {
        write_line("FAIL", "the block did not say where my memory is");
        return 0;
    }

    /* The handshake, asking for the one feature this device has that we use:
     * EDID, the display's own answer to how big its glass is. */
    uint32_t features = 0;
    if (!aegir::virtio::handshake(registers, &features, aegir::virtio::kGpuFeatureEdid)) {
        write_line("FAIL", "the device refused the features we asked for");
        return 0;
    }
    aegir::debug_write("      features: ");
    aegir::debug_write_hex(features);
    aegir::debug_write(features & aegir::virtio::kGpuFeatureEdid ? " (edid among them)\n"
                                                                 : " (no edid)\n");

    /* The control queue, before DRIVER_OK (virtio 1.x, 2.1.1 step 8): one
     * queue, zeroed first, because a nonzero used index is a device that
     * answered a question nobody asked. The cursor queue stays unset. */
    static aegir::virtio::Queue queue;
    volatile uint8_t *queue_page = reinterpret_cast<volatile uint8_t *>(memory_address);
    for (uint32_t i = 0; i < aegir::virtio::kQueueBytes; ++i) {
        queue_page[i] = 0;
    }
    queue.place(queue_page, memory_physical);
    aegir::virtio::QueueReport queue_report{};
    queue.set_up(registers, 0, aegir::virtio::kQueueSize, &queue_report);
    registers.write(aegir::virtio::kStatus,
                    aegir::virtio::kStatusAcknowledge | aegir::virtio::kStatusDriver |
                        aegir::virtio::kStatusFeaturesOk | aegir::virtio::kStatusDriverOk);

    uint32_t const scanouts =
        registers.read(aegir::virtio::kConfig + aegir::virtio::kGpuConfigNumScanouts);
    aegir::debug_write("      config: ");
    aegir::debug_write_unsigned(scanouts);
    aegir::debug_write(" scanouts\n");

    /* The port this driver serves, and the shared window that is the
     * framebuffer: the spawner made and mapped both (aegir/framebuffer.h). */
    aegir::ipc::Owner port = aegir::ipc::Owner::find("port", 4);
    if (!port.valid()) {
        write_line("FAIL", "no port was given to me");
        return 0;
    }
    if (!aegir::bootstrap::shared_window(&g_window_address, &g_window_bytes,
                                         &g_window_physical)) {
        write_line("FAIL", "no shared window was given to me");
        return 0;
    }

    /* What the display is: the first scanout's preferred mode. Honored, not
     * hardcoded -- the device says what it has. */
    aegir::virtio::GetDisplayInfo ask{};
    ask.hdr.type = aegir::virtio::kGpuCmdGetDisplayInfo;
    aegir::virtio::DisplayInfoResponse display{};
    aegir::virtio::GpuResult result =
        aegir::virtio::control(registers, queue, &ask, sizeof(ask), &display,
                               sizeof(display));
    if (!result.completed || result.response_type != aegir::virtio::kGpuRespOkDisplayInfo ||
        display.pmodes[0].r.width == 0 || display.pmodes[0].r.height == 0) {
        write_line("FAIL", "the device would not say what its display is");
        return 0;
    }
    g_width = display.pmodes[0].r.width;
    g_height = display.pmodes[0].r.height;

    /* And how big its glass is, when the feature was negotiated: EDID base
     * block bytes 21 and 22 are the physical size in centimetres. */
    if (features & aegir::virtio::kGpuFeatureEdid) {
        aegir::virtio::GetEdid ask_edid{};
        ask_edid.hdr.type = aegir::virtio::kGpuCmdGetEdid;
        ask_edid.scanout = 0;
        aegir::virtio::EdidResponse edid{};
        result = aegir::virtio::control(registers, queue, &ask_edid, sizeof(ask_edid),
                                        &edid, sizeof(edid));
        static constexpr uint8_t kEdidMagic[8] = {0x00, 0xff, 0xff, 0xff,
                                                  0xff, 0xff, 0xff, 0x00};
        bool valid = result.completed &&
                     result.response_type == aegir::virtio::kGpuRespOkEdid && edid.size >= 128;
        for (uint32_t i = 0; valid && i < sizeof(kEdidMagic); ++i) {
            valid = edid.edid[i] == kEdidMagic[i];
        }
        if (valid) {
            g_phys_width_mm = static_cast<uint32_t>(edid.edid[21]) * 10;
            g_phys_height_mm = static_cast<uint32_t>(edid.edid[22]) * 10;
        }
    }

    /* The instance name heads the marker line: two heads print two lines, and
     * the runner's screen dump keys on which one said it. */
    uint32_t name_length = 0;
    char const *name = aegir::bootstrap::name(&name_length);

    /* The mode's price is the window's size: a display that outgrows the
     * framebuffer is refused here, loudly, rather than scanned out wrong. */
    if (static_cast<uint64_t>(g_width) * g_height * 4 > g_window_bytes) {
        write_line("FAIL", "the display's own mode does not fit my window");
        return 0;
    }
    if (!apply_mode(registers, queue, g_width, g_height)) {
        write_line("FAIL", "the scanout would not come up");
        return 0;
    }
    paint_bands();
    if (!present(registers, queue, 0, 0, g_width, g_height)) {
        write_line("FAIL", "the first frame would not flush");
        return 0;
    }

    /* The interrupt the spawner paired with the device, when it did. */
    uint64_t irq_notification = 0;
    uint64_t irq_handler = 0;
    if (aegir::bootstrap::capability("irq.notify", 10, &irq_notification) &&
        aegir::bootstrap::capability("irq.handler", 11, &irq_handler)) {
        queue.use_interrupts(irq_notification, irq_handler);
    }

    seL4_Signal(aegir::bootstrap::kSlotSupervision);

    aegir::debug_write("      ");
    aegir::debug_write(name, name_length);
    aegir::debug_write(": scanout ");
    aegir::debug_write_unsigned(g_width);
    aegir::debug_write("x");
    aegir::debug_write_unsigned(g_height);
    aegir::debug_write(", physical ");
    aegir::debug_write_unsigned(g_phys_width_mm);
    aegir::debug_write("x");
    aegir::debug_write_unsigned(g_phys_height_mm);
    aegir::debug_write(" mm, the pattern is up\n");

    /* The serve loop. `info` answers from what bring-up learned; `flush`
     * pushes the window as it stands; `set_mode` re-points the scanout,
     * refused only when the mode outgrows the window (aegir/framebuffer.h).
     * Calls serialize at the endpoint, so one outstanding control command is
     * structural, not a lock. */
    for (;;) {
        uint64_t words[4];
        uint32_t count = 0;
        seL4_Word badge = 0;
        uint32_t const method = port.receive_words(words, 4, &count, &badge);
        if (method == aegir::framebuffer::kMethodInfo) {
            uint64_t const answer[] = {g_width, g_height, g_width * 4,
                                       aegir::virtio::kGpuFormatB8G8R8X8, g_phys_width_mm,
                                       g_phys_height_mm};
            port.reply_words(answer, aegir::framebuffer::kInfoWords);
        } else if (method == aegir::framebuffer::kMethodFlush && count == 4) {
            /* The rectangle is clipped to the screen; the driver transfers and
             * flushes only what changed */
            uint32_t const x = words[0] < g_width ? static_cast<uint32_t>(words[0]) : g_width;
            uint32_t const y = words[1] < g_height ? static_cast<uint32_t>(words[1]) : g_height;
            uint32_t const width =
                x + words[2] <= g_width ? static_cast<uint32_t>(words[2]) : g_width - x;
            uint32_t const height =
                y + words[3] <= g_height ? static_cast<uint32_t>(words[3]) : g_height - y;
            (void)present(registers, queue, x, y, width, height);
            port.reply(0);
        } else if (method == aegir::framebuffer::kMethodSetMode && count == 2) {
            uint64_t applied[] = {0, 0};
            if (words[0] != 0 && words[1] != 0 && words[0] <= 0xffffffffu &&
                words[1] <= 0xffffffffu &&
                words[0] * words[1] * 4 <= g_window_bytes &&
                apply_mode(registers, queue, static_cast<uint32_t>(words[0]),
                           static_cast<uint32_t>(words[1]))) {
                paint_bands();
                if (present(registers, queue, 0, 0, g_width, g_height)) {
                    applied[0] = g_width;
                    applied[1] = g_height;
                    aegir::debug_write("      ");
                    aegir::debug_write(name, name_length);
                    aegir::debug_write(": scanout ");
                    aegir::debug_write_unsigned(g_width);
                    aegir::debug_write("x");
                    aegir::debug_write_unsigned(g_height);
                    aegir::debug_write(", the pattern is up\n");
                }
            }
            port.reply_words(applied, 2);
        } else {
            /* A method we do not know is a protocol version we do not speak:
             * the reply says so by saying nothing. */
            port.reply(0);
        }
    }
}
