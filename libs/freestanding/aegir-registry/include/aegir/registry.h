/*
 * The protocol the device manager's registry port serves, and nothing else.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The device manager builds the map -- which device the tree described, which
 * driver the registry row named for it, what the join with director's grants
 * found -- and until now the map existed in its memory and on the console.
 * This port is the map as something a service can *ask* (specs/services.md).
 * The questions are read-only: what authority a device needs moves at spawn
 * time, down the chain, and nothing here hands any out.
 *
 * The wire: `count` answers one word; `describe` takes an index and answers
 * a Row's words, the multi-word form of the port envelope (aegir/ipc);
 * `open` takes an index and answers with one capability -- the bound
 * driver's port, minted with the caller's badge, so a client finds a
 * spawned driver's port without a static edge in the manifest
 * (specs/services.md). `window` answers the shape of the shared window the
 * bound driver's port serves through -- its page bits and page count -- and
 * `window_frame` hands over one of its frames, one cap per reply, from the
 * pristine set kept for exactly this (a cap minted before any mapping can
 * be mapped by the receiver; specs/console.md's pixel slices ride the same
 * shape). Asking for an unbound row, a row with no window, a frame past the
 * count, or a row that does not exist is the empty reply, as is a method
 * the port does not know: a protocol version it does not speak, and the
 * answer says so by saying nothing.
 *
 * `open_bound` below is the walk every client of that takes -- find the
 * bound row by name, open it -- shared now that a session walks it too
 * (specs/auth.md's input path).
 */

#pragma once

#include <aegir/ipc/port.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::registry {

/** The port's name, in the class.instance shape every port is named in. */
constexpr char kPortName[] = "devmgr.registry";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

constexpr uint32_t kMethodCount = 1;    /* answer: how many devices the map holds */
constexpr uint32_t kMethodDescribe = 2; /* in: an index; answer: a Row's words */
constexpr uint32_t kMethodOpen = 3;     /* in: an index; answer: the bound driver's
                                           port, minted with the caller's badge */
constexpr uint32_t kMethodWindow = 4;   /* in: an index; answer: the window's page
                                           bits and page count; empty when the row
                                           declares no window */
constexpr uint32_t kMethodWindowFrame = 5; /* in: an index and a frame index; answer:
                                              that window frame's capability, from the
                                              pristine client set */

/** One device, as the map knows it: what the tree said, which driver the
 *  registry row named, and what the join found. The strings are the map's
 *  own, NUL-terminated within their fields; an empty `instance` is a device
 *  the tree and the registry agree on that nobody drives -- reported, not
 *  driven. */
struct Row {
    char instance[16];   /* "blk.virtio0" -- built when the join succeeds */
    char compatible[16]; /* the tree's string the registry row claims */
    char binary[24];     /* the driver image's name in the initrd */
    uint64_t base;       /* the register window's physical base */
    uint64_t bytes;      /* its size */
    uint64_t irq;        /* the interrupt the tree gives it; 0: none */
    uint64_t window_bits; /* the shared window the driver declared; 0: none */
    uint64_t bound;      /* 1 when a driver was spawned for it */
};

/** The Row as the message carries it. */
constexpr uint32_t kRowWords = (sizeof(Row) + 7) / 8;

/** Walk the map to the bound row named `name`: its index, or -1 when the
 *  map has no such bound row. The index is what `open`, `window` and
 *  `window_frame` take. */
inline int64_t find_bound(aegir::ipc::Consumer const &registry, char const *name,
                          uint32_t name_length) noexcept
{
    aegir::ipc::Reply const count = registry.call(kMethodCount, 0);
    if (count.error != 0) {
        return -1;
    }
    for (uint64_t i = 0; i < count.word; ++i) {
        uint64_t words[kRowWords];
        aegir::ipc::WordsReply const described =
            registry.call_words(kMethodDescribe, &i, 1, words, kRowWords);
        if (described.error != 0 || described.count != kRowWords) {
            return -1;
        }
        auto const *row = reinterpret_cast<Row const *>(words);
        bool is = row->bound != 0 && name_length < sizeof(row->instance) &&
                  row->instance[name_length] == '\0';
        for (uint32_t c = 0; is && c < name_length; ++c) {
            is = row->instance[c] == name[c];
        }
        if (is) {
            return static_cast<int64_t>(i);
        }
    }
    return -1;
}

/** Walk the map to the bound row named `name` and open it: the port the
 *  answer carries lands in `slot`, minted with the caller's own badge
 *  (specs/services.md). False when the map has no such bound row or the
 *  open was refused. */
inline bool open_bound(aegir::ipc::Consumer const &registry, char const *name,
                       uint32_t name_length, seL4_CPtr slot) noexcept
{
    int64_t const index = find_bound(registry, name, name_length);
    if (index < 0) {
        return false;
    }
    uint64_t const at = static_cast<uint64_t>(index);
    bool cap_arrived = false;
    uint64_t in[1];
    aegir::ipc::WordsReply const opened =
        registry.call_transfer(kMethodOpen, &at, 1, 0, in, 1, &cap_arrived);
    return opened.error == 0 && cap_arrived && aegir::ipc::take_received_cap(slot);
}

/** The shape of the shared window the bound row's driver serves through:
 *  its page bits and page count. False when the row declares no window. */
inline bool window_geometry(aegir::ipc::Consumer const &registry, uint64_t index,
                            uint64_t *page_bits, uint64_t *pages) noexcept
{
    uint64_t in[2];
    aegir::ipc::WordsReply const window =
        registry.call_words(kMethodWindow, &index, 1, in, 2);
    if (window.error != 0 || window.count != 2) {
        return false;
    }
    *page_bits = in[0];
    *pages = in[1];
    return true;
}

/** One frame of the bound row's window: the capability lands in `slot`,
 *  from the pristine set, so the receiver may be its first mapping. False
 *  when the row declares no window or the frame index is past the count. */
inline bool window_frame(aegir::ipc::Consumer const &registry, uint64_t index,
                         uint64_t frame, seL4_CPtr slot) noexcept
{
    uint64_t out[2] = {index, frame};
    bool cap_arrived = false;
    uint64_t in[1];
    aegir::ipc::WordsReply const answered =
        registry.call_transfer(kMethodWindowFrame, out, 2, 0, in, 1, &cap_arrived);
    return answered.error == 0 && cap_arrived && aegir::ipc::take_received_cap(slot);
}

}  // namespace aegir::registry
