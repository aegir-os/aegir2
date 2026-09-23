/*
 * aegir-fs-smoke: the hosted wrapper's checks -- implementation.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * This translation unit is the libc++ side and must not include any seL4
 * header (see checks.h). It exercises aegir::filesystem, the hosted wrapper
 * over the freestanding aegir::vfs transport, with std::filesystem's error
 * model: an error_code overload that reports, and a throwing one.
 */

#include "checks.h"

#include <aegir/debug.h>
#include <aegir/filesystem.h>

#include <cstddef>
#include <exception>
#include <system_error>
#include <vector>

namespace aegir::fs_smoke {

namespace {

int g_failed = 0;

void report(bool ok, char const *what)
{
    aegir::debug_write(ok ? "  fs-smoke: ok: " : "  fs-smoke: FAIL: ");
    aegir::debug_write(what);
    aegir::debug_write("\n");
    if (!ok) {
        ++g_failed;
    }
}

void check_volumes()
{
    std::error_code error;
    std::vector<aegir::filesystem::VolumeInfo> listed = aegir::filesystem::volumes(error);
    bool ok = !error && !listed.empty();
    for (aegir::filesystem::VolumeInfo const &volume : listed) {
        ok = ok && !volume.name.empty();
    }
    report(ok, "aegir::filesystem::volumes() names the volumes");

    bool threw = false;
    std::size_t throwing_count = 0;
    try {
        throwing_count = aegir::filesystem::volumes().size();
    } catch (std::exception const &) {
        threw = true;
    }
    report(!threw && throwing_count == listed.size(),
           "the throwing volumes() agrees with the error_code one");
}

}  // namespace

int run()
{
    check_volumes();
    return g_failed;
}

}  // namespace aegir::fs_smoke
