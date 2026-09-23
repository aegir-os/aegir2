/*
 * The virtio status handshake (virtio 1.x, 2.1.1).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Moved out of aegir-virtio-blk when the second driver arrived: the sequence is
 * the same for every device on the bus, and only the config space and the
 * queues after it are the device's own (specs/services.md).
 */

#pragma once

#include <aegir/virtio/mmio.h>
#include <stdint.h>

namespace aegir::virtio {

/** The status handshake (virtio 1.x, 2.1.1). The device is told, in order, that we have
 *  seen it, that we know how to drive it, and what features we will use; it then either
 *  accepts the feature set -- leaving FEATURES_OK set -- or clears the bit to say it will
 *  not work with us. `features_out`, when given, is the device's low feature word.
 *  `wanted` is the low feature bits this driver would use: only bits the device
 *  *offers* are written back, so a driver asks for its device's own features
 *  (the gpu's VIRTIO_GPU_F_EDID) and the ones that came first ask for nothing. */
bool handshake(Registers const &registers, uint32_t *features_out,
               uint32_t wanted = 0) noexcept;

}  // namespace aegir::virtio
