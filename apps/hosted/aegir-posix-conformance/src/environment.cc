/*
 * aegir-posix-conformance: the environment area.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Every case is derived from the page named beside it in
 * third_party/posix-spec/issue8 (fetched by `make deps`). These interfaces are
 * libc over the process's own environment array, which the startup frame seeds
 * (specs/environment.md), so there is no volume and no syscall behind them.
 */

#include "harness.h"
#include "scratch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

namespace aegir_posix_conformance {
namespace {

bool is(char const *text, char const *want)
{
    size_t const n = length_of(want);
    return equals(text, want, n) && text[n] == '\0';
}

void conforms_or_deviation(char const *interface, char const *test, char const *clause,
                           bool conforms, int observed_errno, char const *expected)
{
    if (conforms) {
        check(interface, test, true);
    } else {
        char reason[128];
        snprintf(reason, sizeof(reason), "%s names %s; Aegir answers errno %d", clause, expected,
                 observed_errno);
        deviation(interface, test, reason);
    }
}

}  // namespace

void environment_surface()
{
    section("environment");

    /* setenv.html RETURN VALUE (zero) and ERRORS ("[EINVAL] The envname
     * argument points to an empty string or points to a string containing an
     * '=' character"). */
    check("setenv", "a variable is set", setenv("AEGIR_CONFORMANCE", "value", 1) == 0);
    errno = 0;
    check("setenv", "an empty name is EINVAL", setenv("", "value", 1) == -1 && errno == EINVAL);
    errno = 0;
    check("setenv", "a name containing '=' is EINVAL",
          setenv("AEGIR=B", "value", 1) == -1 && errno == EINVAL);

    /* getenv.html RETURN VALUE: the value, or a null pointer when the name is
     * not in the environment; no errors are defined. */
    char const *const value = getenv("AEGIR_CONFORMANCE");
    check("getenv", "the value is answered", value != nullptr && is(value, "value"));
    check("getenv", "a name that is not set is null", getenv("AEGIR_CONFORMANCE_NOPE") == nullptr);

    /* unsetenv.html RETURN VALUE (zero) and ERRORS (the same EINVAL clause). */
    check("unsetenv", "a variable is removed", unsetenv("AEGIR_CONFORMANCE") == 0);
    check("getenv", "a removed variable is null", getenv("AEGIR_CONFORMANCE") == nullptr);
    errno = 0;
    check("unsetenv", "an empty name is EINVAL", unsetenv("") == -1 && errno == EINVAL);

    /* putenv.html RETURN VALUE (zero) and ERRORS ("[EINVAL] The string argument
     * points to a string that is not of the form "name=value"" -- may fail). */
    check("putenv", "a name=value string is added", putenv((char *)"AEGIR_PUT=here") == 0);
    char const *const put = getenv("AEGIR_PUT");
    check("getenv", "the putenv value is answered", put != nullptr && is(put, "here"));
    (void)unsetenv("AEGIR_PUT");

    /* environ.html: the array is the process's own, and a variable this process
     * sets is in it. */
    check("environ", "the environment array is present", environ != nullptr);
    (void)setenv("AEGIR_ENVIRON", "yes", 1);
    bool found = false;
    for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        if (is(*entry, "AEGIR_ENVIRON=yes")) {
            found = true;
            break;
        }
    }
    check("environ", "a variable set in this process is in the array", found);
    (void)unsetenv("AEGIR_ENVIRON");

    /* confstr.html RETURN VALUE (the size of the value, or zero when the name
     * has no configuration-defined value) and ERRORS ("[EINVAL] The value of the
     * name argument is invalid"). */
    char buffer[256];
    errno = 0;
    check("confstr", "an invalid name is EINVAL",
          confstr(-1, buffer, sizeof(buffer)) == 0 && errno == EINVAL);
    {
        /* _CS_PATH is the one name the standard requires a value for. */
        size_t const path_size = confstr(_CS_PATH, buffer, sizeof(buffer));
        if (path_size == 0) {
            conforms_or_deviation("confstr", "_CS_PATH has a value", "confstr.html", false, 0,
                                  "_CS_PATH's size");
        } else {
            check("confstr", "_CS_PATH has a value", buffer[0] != '\0');
        }
    }
}

}  // namespace aegir_posix_conformance
