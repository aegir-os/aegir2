/*
 * The entropy port's protocol: what an rng driver serves.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One header, no code, the same shape as aegir/block.h: the driver includes
 * it to serve, a client includes it to call, and neither has to guess what
 * the other meant. Entropy is small -- a nonce is tens of bytes -- so the
 * answer rides in the port's own envelope and there is no shared window: a
 * registry row for such a driver says `window=0` (specs/services.md).
 */

#ifndef AEGIR_ENTROPY_H
#define AEGIR_ENTROPY_H

#include <aegir/ipc/port.h>
#include <stdint.h>

namespace aegir::entropy {

/** Read: the word is how many bytes are wanted. The answer's words carry
 *  what the device filled -- the used length is the truth, because the
 *  device may fill less than was posted -- and a device that answered
 *  nothing is an empty reply. */
constexpr uint32_t kMethodRead = 1;

/** The read's ceiling, in bytes: what the envelope carries
 *  (aegir/ipc/port.h). Asking for more is not an error; it is this many. */
constexpr uint32_t kMaxBytes = aegir::ipc::kMaxWords * 8;

}  // namespace aegir::entropy

#endif  // AEGIR_ENTROPY_H
