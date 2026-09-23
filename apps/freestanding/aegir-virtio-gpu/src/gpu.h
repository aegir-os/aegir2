/*
 * The gpu's control commands -- the device-specific shapes. See src/gpu.cc.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Everything the virtio transport shares -- the register window, the
 * handshake, the queue -- is libs/aegir-virtio's; what lives here is only
 * what a gpu is: the command and response layouts of the control queue
 * (virtio 1.x, 5.7.6), the command numbers, and the one command runner.
 * The same seam as the block driver's sector.h.
 */

#ifndef AEGIR_VIRTIO_GPU_GPU_H
#define AEGIR_VIRTIO_GPU_GPU_H

#include <aegir/virtio/queue.h>
#include <stdint.h>

namespace aegir::virtio {

/* The control commands, and only they -- the cursor queue is a second queue
 * this driver does not set up (virtio 1.x, 5.7.6). */
constexpr uint32_t kGpuCmdGetDisplayInfo = 0x0100;
constexpr uint32_t kGpuCmdResourceCreate2d = 0x0101;
constexpr uint32_t kGpuCmdResourceUnref = 0x102;
constexpr uint32_t kGpuCmdSetScanout = 0x103;
constexpr uint32_t kGpuCmdResourceFlush = 0x104;
constexpr uint32_t kGpuCmdTransferToHost2d = 0x105;
constexpr uint32_t kGpuCmdResourceAttachBacking = 0x106;
constexpr uint32_t kGpuCmdGetEdid = 0x10a;

/* The answers. OK_NODATA is the yes for every command with nothing to say;
 * the ERRs beyond UNSPEC exist, but a driver that reads the type word needs
 * only "not the OK I asked for" to fail loudly. */
constexpr uint32_t kGpuRespOkNodata = 0x1100;
constexpr uint32_t kGpuRespOkDisplayInfo = 0x1101;
constexpr uint32_t kGpuRespOkEdid = 0x1104;

/* The one format this driver asks for: bytes B, G, R, X in memory, so a
 * pixel written as the little-endian word 0x00RRGGBB is what the screen
 * shows (virtio 1.x, 5.7.6.6). The framebuffer protocol answers with the
 * same number (aegir/framebuffer.h). */
constexpr uint32_t kGpuFormatB8G8R8X8 = 2;

/* VIRTIO_GPU_F_EDID: the device has an EDID block per scanout to answer
 * GET_EDID with (virtio 1.2, 5.7.6.8). The first low feature bit any driver
 * here negotiates. */
constexpr uint32_t kGpuFeatureEdid = 1u << 1;

/* The config space past the transport's registers (virtio 1.x, 5.7.4):
 * events_read and events_clear first, then the counts. */
constexpr uint32_t kGpuConfigEventsRead = 0x00;
constexpr uint32_t kGpuConfigNumScanouts = 0x08;

/* Every command and response starts with this (virtio 1.x, 5.7.6.1): the
 * type word is what both directions switch on; the rest is zero here --
 * no fences, no contexts. */
struct CtrlHeader {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
};

struct GpuRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct GetDisplayInfo {
    CtrlHeader hdr;
};

struct DisplayInfoResponse {
    CtrlHeader hdr;
    struct {
        GpuRect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[16]; /* VIRTIO_GPU_MAX_SCANOUTS */
};

struct Create2d {
    CtrlHeader hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct AttachBacking {
    CtrlHeader hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    /* The entries, of which this driver sends exactly one: the framebuffer
     * is the shared window, one contiguous run of mega pages. */
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

struct SetScanout {
    CtrlHeader hdr;
    GpuRect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct TransferToHost2d {
    CtrlHeader hdr;
    GpuRect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct Flush {
    CtrlHeader hdr;
    GpuRect r;
    uint32_t resource_id;
    uint32_t padding;
};

struct Unref {
    CtrlHeader hdr;
    uint32_t resource_id;
    uint32_t padding;
};

struct GetEdid {
    CtrlHeader hdr;
    uint32_t scanout;
    uint32_t padding;
};

struct EdidResponse {
    CtrlHeader hdr;
    uint32_t size;
    uint32_t padding;
    uint8_t edid[1024];
};

/* One answer to one command: `completed` is the transport's part (the used
 * ring moved), `response_type` is the device's own answer -- the caller
 * checks it against the OK it asked for. */
struct GpuResult {
    bool completed;
    uint32_t response_type;
    uint32_t used_bytes;
};

/** Run one control command: `command`/`command_bytes` device-readable, the
 *  answer device-writable into `response`/`response_bytes`. One chain of two
 *  descriptors, one command outstanding at a time -- the port serializes
 *  callers, and the self-test is sequential. */
GpuResult control(Registers const &registers, Queue &queue, void const *command,
                  uint32_t command_bytes, void *response, uint32_t response_bytes) noexcept;

}  // namespace aegir::virtio

#endif  // AEGIR_VIRTIO_GPU_GPU_H
