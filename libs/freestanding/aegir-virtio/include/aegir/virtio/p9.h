/*
 * virtio-9p: the transport's own shapes -- its queue, its feature, and the
 * config space that carries the export's name.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A virtio-9p device (id 9) is a 9P filesystem offered to the guest: the
 * device carries T-messages to a host export and R-messages back (specs/9p.md).
 * Everything shared with every other virtio device -- the register window, the
 * handshake, the virtqueue -- is in mmio.h, handshake.h and queue.h; what is
 * the device's own is here: one request queue, one feature bit, and a config
 * space that is the mount tag, which is the export's name.
 *
 * Each request is a two-descriptor chain: a device-readable buffer holding the
 * T-message, then a device-writable buffer the R-message lands in. QEMU reads
 * the seven-byte 9P header off the first descriptor and writes the reply to
 * the second (hw/9pfs/virtio-9p-device.c's handle_9p_output), which is why the
 * caller supplies both halves rather than one buffer written in place.
 */

#pragma once

#include <stdint.h>

namespace aegir::virtio {

/** The one feature this driver accepts: the config space names the export
 *  (VIRTIO_9P_MOUNT_TAG). Without it there is no tag to mount. */
constexpr uint32_t kP9FeatureMountTag = 1;

/** The device has one queue, for requests. */
constexpr uint32_t kP9QueueRequest = 0;

/** The config space, from the register window's kConfig: a 16-bit tag length
 *  then that many bytes of tag, no terminator. */
constexpr uint32_t kP9ConfigTagLen = 0;
constexpr uint32_t kP9ConfigTag = 2;

/** The longest tag QEMU will write before the config space runs into the rest
 *  of the page; a tag past it is refused rather than truncated silently. */
constexpr uint32_t kP9TagMax = 36;

}  // namespace aegir::virtio
