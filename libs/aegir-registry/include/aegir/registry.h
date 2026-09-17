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
 * a Row's words, the multi-word form of the port envelope (aegir/ipc). A
 * method the port does not know is a protocol version it does not speak,
 * and the answer says so by saying nothing.
 */

#pragma once

#include <stdint.h>

namespace aegir::registry {

/** The port's name, in the class.instance shape every port is named in. */
constexpr char kPortName[] = "devmgr.registry";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

constexpr uint32_t kMethodCount = 1;    /* answer: how many devices the map holds */
constexpr uint32_t kMethodDescribe = 2; /* in: an index; answer: a Row's words */

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

}  // namespace aegir::registry
