/*
 * aegir/release.h: what this system is, in one place (specs/environment.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The words a program is given when it asks the system to identify itself, kept
 * here so that one answer serves every reader: the POSIX layer's `uname` today
 * (specs/posix.md), and whatever else wants to say what this is -- a boot
 * banner, a shell's own `version` -- without inventing its own. Nothing else in
 * the tree read a version string before this, which is why it starts here rather
 * than being threaded through the build.
 *
 * The machine is the one part the architecture decides, and the architecture is
 * the target's business (AGENTS.md: architecture-specific code is abstracted),
 * so it names itself per target and a target it does not know is a compile error
 * rather than a wrong answer.
 */

#pragma once

namespace aegir::release {

/** The system's name: what `uname`'s sysname answers. */
constexpr char const *kSystem = "Aegir";

/** The machine's own name: what `uname`'s nodename answers. */
constexpr char const *kNode = "aegir";

/** The release this is: what `uname`'s release answers. */
constexpr char const *kVersion = "0.1";

#if defined(__riscv) && __riscv_xlen == 64

/** What this is, in one line: what `uname`'s version answers. */
constexpr char const *kDescription = "Aegir riscv64";

/** The architecture: what `uname`'s machine answers. */
constexpr char const *kMachine = "riscv64";

#elif defined(__aarch64__)

constexpr char const *kDescription = "Aegir aarch64";
constexpr char const *kMachine = "aarch64";

#elif defined(__x86_64__)

constexpr char const *kDescription = "Aegir x86-64";
constexpr char const *kMachine = "x86-64";

#else

#error "aegir/release.h: no machine name for this architecture"

#endif

}  // namespace aegir::release
