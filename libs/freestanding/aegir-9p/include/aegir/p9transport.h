/*
 * The port a 9P transport serves: one request, one reply, through a window.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A 9P transport carries two buffers -- the T-message out and the R-message
 * back -- and where the codec (aegir/9p.h) builds and reads them, this is the
 * contract for moving them (specs/9p.md). The transport driver serves one
 * method, `round-trip`: the caller has written a request into the window's
 * first half and calls with its length; the driver carries it and answers the
 * reply's length, and the caller reads the reply from the window's second
 * half. There is no session in the port -- the 9P session (the version, the
 * fids, the attached tree) is the client's, which is what keeps the same
 * transport usable by any 9P client.
 *
 * The window is split in half so neither side copies: the driver points the
 * device's two descriptors at the halves' *physical* addresses, reads the
 * request where the caller left it, and the device writes the reply where the
 * caller will find it. Both sides know the window's size -- the driver from
 * its grant, the client from the registry's geometry -- so the split is a
 * rule (`request_bytes` below) and not a number either has to be told.
 *
 * The tag a virtio-9p transport carries names its export, and `info` is how a
 * client learns it (the volume is named after it, specs/9p.md). It is the
 * namespace's string shape, the same one every port carries text in.
 */

#pragma once

#include <aegir/nmspace.h>
#include <stdint.h>

namespace aegir::p9transport {

/** The port a transport driver serves, named "port" in its bootstrap block. */
constexpr char kPortName[] = "port";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

/** info: no words; the answer is the mount tag as a string (aegir/nmspace.h)
 *  -- what the export is called. A transport with no tag answers nothing. */
constexpr uint32_t kMethodInfo = 1;

/** round-trip: the request's length in bytes. The request is at the window's
 *  start, the reply is read from `reply_offset`. The answer is the reply's
 *  length, or zero when the device did not answer. */
constexpr uint32_t kMethodRoundTrip = 2;

/** The longest tag a virtio-9p transport carries: the device config's field. */
constexpr uint32_t kTagMax = 36;

/** The window's first half is the request; its second is the reply. Each side
 *  computes the split from the window's own size, so a larger window is a
 *  larger message and never a second number to agree on. */
constexpr uint32_t request_bytes(uint32_t window_bytes) noexcept
{
    return window_bytes / 2;
}

constexpr uint32_t reply_offset(uint32_t window_bytes) noexcept
{
    return window_bytes / 2;
}

constexpr uint32_t reply_bytes(uint32_t window_bytes) noexcept
{
    return window_bytes - window_bytes / 2;
}

}  // namespace aegir::p9transport
