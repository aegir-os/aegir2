/*
 * aegir-env-smoke: the process environment checks -- implementation.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The libc++ side: it includes <aegir/environment.h>, which pulls in libc++'s
 * <string_view>, so it must not include an seL4 header (checks.h, and the
 * boundary specs/userland.md records).
 */

#include "checks.h"

#include <aegir/debug.h>
#include <aegir/environment.h>

#include <cstdlib>
#include <cstring>
#include <string_view>

namespace aegir::env_smoke {

namespace {

int g_failed = 0;

void report(bool ok, char const *what)
{
    aegir::debug_write(ok ? "  env-smoke: ok: " : "  env-smoke: FAIL: ");
    aegir::debug_write(what);
    aegir::debug_write("\n");
    if (!ok) {
        ++g_failed;
    }
}

bool equals(std::string_view value, char const *text)
{
    return value == std::string_view(text);
}

void check_arguments()
{
    using aegir::environment::argc;
    using aegir::environment::argv;
    report(argc() == 3, "argc is 3");
    char const *const *args = argv();
    report(args != nullptr && args[0] != nullptr && args[0][0] != '\0',
           "argv[0] is the program's name");
    report(args != nullptr && args[1] != nullptr && std::strcmp(args[1], "alpha") == 0,
           "argv[1] is the first argument");
    report(args != nullptr && args[2] != nullptr && std::strcmp(args[2], "beta") == 0,
           "argv[2] is the second argument");
}

void check_environment()
{
    using aegir::environment::getenv;
    using aegir::environment::setenv;
    char const *inherited = getenv("AEGIR");
    report(inherited != nullptr && std::strcmp(inherited, "smoke") == 0,
           "getenv reads an inherited value");
    report(setenv("OWN", "set"), "setenv takes a new name");
    char const *own = getenv("OWN");
    report(own != nullptr && std::strcmp(own, "set") == 0, "getenv reads the process's own value");
    report(setenv("AEGIR", "own"), "setenv shadows an inherited name");
    char const *shadowed = getenv("AEGIR");
    report(shadowed != nullptr && std::strcmp(shadowed, "own") == 0,
           "the process's own value shadows the inherited one");
}

void check_current_dir()
{
    using aegir::environment::current_dir;
    using aegir::environment::set_current_dir;
    report(equals(current_dir(), "Sys:"), "the current directory is the spawner's");
    report(set_current_dir("Work:"), "set_current_dir takes a path");
    report(equals(current_dir(), "Work:"), "the current directory changed");
}

}  // namespace

int run()
{
    void *raw = std::malloc(4096);
    report(raw != nullptr, "malloc returns memory");
    std::free(raw);
    check_arguments();
    check_environment();
    check_current_dir();
    return g_failed;
}

}  // namespace aegir::env_smoke
