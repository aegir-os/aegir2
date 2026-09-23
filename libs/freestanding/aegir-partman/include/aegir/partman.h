/*
 * The protocol the partition manager's own port serves, and nothing else.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A filesystem the partition manager starts is not in the manifest, so it
 * cannot declare what it needs -- it is *given* its ports, and one of them
 * is the caller half of this one. The port exists for the one thing the
 * manager cannot know without asking: the volume's *label*, which lives in
 * the filesystem's own structures (specs/vfs.md).
 *
 *   - `announce`: words carry the label. The manager registers the label
 *     with the VFS, together with the volume port's caller half it kept at
 *     the spawn, and answers with the name the volume actually got -- a
 *     duplicate gains a `_N` suffix at the VFS, and the filesystem is told.
 *
 * A new filesystem type implements announce and the volume protocol, and
 * nothing about the VFS.
 */

#pragma once

#include <stdint.h>

namespace aegir::partman {

/** The port's name, in the class.instance shape every port is named in. */
constexpr char kPortName[] = "partman.partitions";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

constexpr uint32_t kMethodAnnounce = 1; /* in: label words; answer: assigned name */

}  // namespace aegir::partman
