/*
 * virtio-input's device config space: how a transport says what kind of
 * input device sits behind it.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * virtio-input (device id 18) is a family, not a device: the keyboard, the
 * mouse and the tablet all answer to the same id, and the kind lives here --
 * the config window's selector registers choose what the 128 data bytes mean
 * (virtio 1.x, 5.8.4). The driver reads them to announce what it is; the
 * device manager's probe reads EV_BITS to place a transport on the right
 * registry row (the `evtype` key, specs/services.md).
 */

#pragma once

#include <stdint.h>

namespace aegir::virtio::input {

/* The selector registers, as byte offsets into the config window (which
 * itself starts at kConfig in the transport's register page): select says
 * what the data page holds, subsel which page of it, size how much of it is
 * real. All byte-sized -- the selectors work at byte granularity. */
constexpr uint32_t kRegSelect = 0;
constexpr uint32_t kRegSubsel = 1;
constexpr uint32_t kRegSize = 2;
constexpr uint32_t kRegData = 8;

/* The selects (virtio 1.x, 5.8.4). EV_BITS' subsel is an event type
 * (aegir/input.h's kEv* values) and the data is the bitmap of codes the
 * device raises for it -- a zero size says the type never comes. ABS_INFO's
 * subsel is an axis and the data is its absinfo. */
constexpr uint8_t kSelectNone = 0x00;
constexpr uint8_t kSelectIdName = 0x01;
constexpr uint8_t kSelectIdSerial = 0x02;
constexpr uint8_t kSelectIdDevids = 0x03;
constexpr uint8_t kSelectPropBits = 0x10;
constexpr uint8_t kSelectEvBits = 0x11;
constexpr uint8_t kSelectAbsInfo = 0x12;

/* absinfo's fields, as byte offsets into the data page: five little-endian
 * 32-bit values -- min, max, fuzz, flat, resolution. */
constexpr uint32_t kAbsMin = 0;
constexpr uint32_t kAbsMax = 4;

/** Read a little-endian 32-bit value out of a data page. Not constexpr: the
 *  page is volatile, and a volatile read is not a constant expression. */
inline uint32_t le32(volatile uint8_t const *data, uint32_t offset) noexcept
{
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

}  // namespace aegir::virtio::input

