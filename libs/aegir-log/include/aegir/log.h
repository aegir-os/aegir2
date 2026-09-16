/*
 * The logger's port, as its callers see it.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A port's wire format belongs to the service that owns it (specs/services.md),
 * so this is where the logger's lives: the port's name, the method, and the
 * events it knows how to render. A service that wants to log links this and does
 * not have to know what `logger` is made of -- which is the point, and the shape
 * every service's protocol will take.
 */

#ifndef AEGIR_LOG_H
#define AEGIR_LOG_H

#include <stdint.h>

namespace aegir::log {

/** The port `logger` owns. Exactly one service owns it, and only that service may
 *  receive on it (specs/services.md). */
constexpr char const kPortName[] = "log.main";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

/** Method 1: an event happened. One word: the event code. */
constexpr uint32_t kMethodEvent = 1;

/** What the logger knows how to say. The logger reports the caller's badge
 *  alongside, so an event is "who" as well as "what" -- and words rather than
 *  strings is a v1 choice with a reason: a string is an out-of-line buffer, and
 *  that convention deserves designing rather than improvising. */
enum class Event : uint64_t {
    /** A service has finished starting and is about to report ready. */
    Starting = 1,
    /** A service is ready. */
    Ready = 2,
    /** The boot set this service belongs to has been created. */
    BootSetCreated = 3,
};

/** The reply the logger gives: zero when the event was recorded. */
constexpr uint64_t kRecorded = 0;

}  // namespace aegir::log

#endif  // AEGIR_LOG_H
