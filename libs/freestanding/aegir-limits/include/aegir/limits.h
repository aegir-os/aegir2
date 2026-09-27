/*
 * Resource limits, configuration (specs/limits.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The file is the system's universal configuration format -- the one
 * services.manifest already uses: [section] headers, key = value lines, #
 * comments, values read to the end of the line (specs/services.md). The
 * service that enforces a resource reads it and resolves each subject's
 * rules; the format has no second syntax and no runtime parser to grow.
 *
 * This library is the pure reader: no allocation, no seL4, no libc. `parse`
 * validates the whole file (unknown sections and keys are errors, as in the
 * manifest) and keeps the view; `resolve` answers the most specific rule for a
 * subject by scanning that view, so the caller carries no rule capacity of its
 * own. The precedence is user, then class, then default; a pair no subject
 * states is unlimited -- the machine.
 */

#ifndef AEGIR_LIMITS_H
#define AEGIR_LIMITS_H

#include <stdint.h>

namespace aegir::limits {

/** A range of the limits text. Never owned, never NUL-terminated. */
struct View {
    char const *data;
    uint32_t length;
};

/** The resource a rule meters. Memory is the first, and only, one
 *  (specs/limits.md): committed bytes. */
enum class Resource : uint8_t { Memory };

/** What a rule does when a subject crosses it. `log` records the crossing and
 *  lets it through; `deny` refuses the allocation that crosses it. */
enum class Action : uint8_t { Log, Deny };

/** One resolved rule: whether the subject stated one, and the amount. */
struct Amount {
    bool set;
    uint64_t bytes;
};

/** Whether a subject holding `committed` bytes, asking for `want` more, crosses
 *  `amount`: the allocation that pushes it over. False when no rule is set, and
 *  false at exactly the amount -- the limit is the largest allowed total, not
 *  the first refused one. The service's `alloc` is the caller: a deny refuses
 *  the allocation this is true for, and a log records it (specs/limits.md). */
bool crosses(Amount amount, uint64_t committed, uint64_t want) noexcept;

class Limits {
public:
    struct Problem {
        uint32_t line;
        char const *message;
    };

    /** Validate `text` as the limits format and keep it for resolve. False
     *  means problem() says why -- an unknown section or key, a malformed
     *  amount, or a line that is neither. */
    bool parse(char const *text, uint32_t length) noexcept;

    Problem problem() const noexcept { return problem_; }
    bool valid() const noexcept { return valid_; }

    /** The most specific amount for one subject and pair. `user` and `klass`
     *  are the names from the user database: a user:<user> rule wins over a
     *  class:<klass> rule, which wins over default. `set` false means no rule
     *  -- unlimited. */
    Amount resolve(View user, View klass, Resource resource, Action action) const noexcept;

    static constexpr uint8_t kPrecedenceUser = 3;
    static constexpr uint8_t kPrecedenceClass = 2;
    static constexpr uint8_t kPrecedenceDefault = 1;

private:
    char const *text_;
    uint32_t length_;
    Problem problem_;
    bool valid_;
};

}  // namespace aegir::limits

#endif  // AEGIR_LIMITS_H
