/*
 * The process environment (specs/environment.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * What a process was given at spawn: its arguments, its environment, and its
 * current directory. The first two are the startup frame the runtime already
 * carries (sel4runtime's argc/argv/envp); the current directory is a bootstrap
 * block entry. A program includes this and never reads the frame, the block or
 * a slot itself (specs/userland.md: the calls belong in a library).
 */

#ifndef AEGIR_ENVIRONMENT_H
#define AEGIR_ENVIRONMENT_H

#include <string_view>

namespace aegir::environment {

/** How many arguments the process was started with; argv[0] is its name. */
int argc() noexcept;

/** The argument vector: `argc()` NUL-terminated strings, then a null. */
char const *const *argv() noexcept;

/** The value of `name`, or nullptr. The process's own settings first, then the
 *  environment it inherited. */
char const *getenv(char const *name) noexcept;

/** Set `name` to `value` in the process's own environment. False when the name
 *  or the value is null, or the name is empty or contains '='. */
bool setenv(char const *name, char const *value) noexcept;

/** The whole environment as `NAME=VALUE` strings, NUL-terminated, the
 *  process's own settings shadowing what it inherited -- what a spawner passes
 *  on (inheritance is the default, specs/environment.md). The view is this
 *  library's and is rebuilt on each call; it is valid until the next call to
 *  setenv or environ. */
char const *const *environ() noexcept;

/** The current directory, or an empty view when the process has none. */
std::string_view current_dir() noexcept;

/** Set the current directory (a VFS path, `Volume:component/path`). An empty
 *  path clears it, so a relative path is then refused (specs/environment.md). */
bool set_current_dir(std::string_view path) noexcept;

}  // namespace aegir::environment

#endif  // AEGIR_ENVIRONMENT_H
