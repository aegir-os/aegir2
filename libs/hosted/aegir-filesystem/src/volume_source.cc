/*
 * aegir::filesystem's bridge to the freestanding transport -- implementation.
 * See volume_source.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * This translation unit is the seL4 side: it includes the transport and must
 * not include a libc++ header. It reports the namespace's volumes in plain
 * types the libc++ side can take.
 */

#include "volume_source.h"

#include <aegir/nmspace.h>
#include <aegir/vfs.h>

namespace aegir::filesystem::internal {

bool volume_count(uint64_t &count) noexcept
{
    aegir::vfs::Namespace const space = aegir::vfs::Namespace::find();
    if (!space.valid()) {
        return false;
    }
    return space.volume_count(count);
}

bool describe(uint64_t index, char *name, uint32_t capacity, uint32_t &name_length,
              uint64_t &flags, uint64_t &bound) noexcept
{
    aegir::vfs::Namespace const space = aegir::vfs::Namespace::find();
    if (!space.valid()) {
        return false;
    }
    aegir::nmspace::Row row{};
    if (!space.describe(index, row)) {
        return false;
    }
    name_length = 0;
    for (uint32_t i = 0; i < aegir::nmspace::kNameMax && row.name[i] != '\0'; ++i) {
        if (name_length < capacity) {
            name[name_length] = row.name[i];
        }
        ++name_length;
    }
    flags = row.flags;
    bound = row.bound;
    return true;
}

}  // namespace aegir::filesystem::internal
