/*
 * aegir-posix-conformance: the case harness.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One function per kind of line the suite prints. The first argument of `check`,
 * `unavailable`, `deviation` and `skip` is the interface's name in
 * `manifests/posix-surface.toml`; `scripts/check_posix_coverage.py` reads those
 * names out of the source, so an interface with no case is a failed check
 * (specs/posix-conformance.md).
 */

#ifndef AEGIR_POSIX_CONFORMANCE_HARNESS_H
#define AEGIR_POSIX_CONFORMANCE_HARNESS_H

#include <stdbool.h>

namespace aegir_posix_conformance {

/* A banner for one area, so the log reads in sections. */
void section(char const *area);

/* An implemented interface, asserted against the standard. */
void check(char const *interface, char const *test, bool ok);

/* A hole the standard names and Aegir does not fill: the call must fail, and the
 * errno is printed so the hole is a number and not a promise. The day the call
 * stops failing, this goes red. */
void unavailable(char const *interface, char const *test, bool failed_call, int err);

/* A deliberate non-conformance (specs/posix.md): named and counted. */
void deviation(char const *interface, char const *test, char const *reason);

/* Not testable in this process, with the reason. */
void skip(char const *interface, char const *test, char const *reason);

/* The tallies the marker prints. */
int passed();
int failed();
int skipped();
int holes();
int deviations();

}  // namespace aegir_posix_conformance

#endif
