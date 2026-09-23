/*
 * aegir::filesystem -- implementation. See include/aegir/filesystem.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * This translation unit is the libc++ side and must not include an seL4 header
 * (volume_source.h explains why the transport calls live behind a bridge).
 * The one thing here so far is the enumeration std::filesystem has no path
 * for: the volumes the VFS's namespace holds. It is read-only and needs no
 * capability of its own -- count and describe are answered on the namespace
 * port -- so it adds nothing to the process's CSpace. The path operations that
 * do need a resolved volume capability arrive with the runtime's file calls,
 * which is where the capability's slot is decided.
 */

#include <aegir/filesystem.h>

#include "volume_source.h"

#include <aegir/nmspace.h>

#include <utility>

namespace aegir::filesystem {

namespace {

std::error_code refused() noexcept
{
    return std::make_error_code(std::errc::io_error);
}

}  // namespace

std::vector<VolumeInfo> volumes(std::error_code &error)
{
    error.clear();

    uint64_t count = 0;
    if (!internal::volume_count(count)) {
        error = refused();
        return {};
    }

    std::vector<VolumeInfo> result;
    result.reserve(static_cast<std::size_t>(count));
    for (uint64_t index = 0; index < count; ++index) {
        char name[aegir::nmspace::kNameMax] = {};
        uint32_t name_length = 0;
        uint64_t flags = 0;
        uint64_t bound = 0;
        if (!internal::describe(index, name, sizeof(name), name_length, flags, bound)) {
            error = refused();
            return result;
        }
        VolumeInfo info;
        info.name.assign(name, name_length < sizeof(name) ? name_length : sizeof(name));
        info.read_only = (flags & aegir::nmspace::kFlagReadOnly) != 0;
        info.bound = bound != 0;
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<VolumeInfo> volumes()
{
    std::error_code error;
    std::vector<VolumeInfo> result = volumes(error);
    if (error) {
        throw std::system_error(error, "aegir::filesystem::volumes");
    }
    return result;
}

}  // namespace aegir::filesystem
