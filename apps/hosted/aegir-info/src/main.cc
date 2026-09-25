/*
 * info: describe the mounted volumes (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The volumes the caller may resolve, with their filesystem type and status.
 * The namespace's count/describe already answer exactly this -- a volume the
 * caller may not resolve is not counted -- so the command only lays the rows
 * out. Free space is not here: it waits on the filesystem's Avail
 * (specs/dos.md's What this is not). A DEVICE narrows the list to one volume.
 */

#include <aegir/args.h>
#include <aegir/command.h>
#include <aegir/nmspace.h>
#include <aegir/vfs.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

/* A volume name as the caller wrote it against a Row's name, ignoring case --
 * Amiga volume names are case-insensitive (specs/vfs.md). */
bool name_matches(char const *wanted, char const *actual) noexcept
{
    for (uint32_t i = 0;; ++i) {
        char a = wanted[i];
        char b = actual[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
        if (a == '\0') {
            return true;
        }
    }
}

void print_row(aegir::nmspace::Row const &row) noexcept
{
    char const *const type = row.type[0] != '\0' ? row.type : "-";
    char status[48];
    std::snprintf(status, sizeof(status), "%s%s%s",
                  (row.flags & aegir::nmspace::kFlagReadOnly) != 0 ? "read-only"
                                                                   : "read/write",
                  (row.flags & aegir::nmspace::kFlagBoot) != 0 ? " boot" : "",
                  (row.flags & aegir::nmspace::kFlagPublic) != 0 ? " public" : "");
    std::printf("%-16s %-8s %s\n", row.name, type, status);
}

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("info")) {
        std::_Exit(127);
    }
    aegir::args::Result const args = aegir::args::read("DEVICE", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "info: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    aegir::vfs::Namespace const space = aegir::vfs::Namespace::find();
    if (!space.valid()) {
        std::fprintf(stderr, "info: no namespace\n");
        return 10;
    }
    uint64_t total = 0;
    if (!space.volume_count(total)) {
        std::fprintf(stderr, "info: cannot read the volume list\n");
        return 10;
    }
    char const *const device = args.value("DEVICE");
    bool shown = false;
    for (uint64_t i = 0; i < total; ++i) {
        aegir::nmspace::Row row{};
        if (!space.describe(i, row)) {
            continue;
        }
        if (device != nullptr && !name_matches(device, row.name)) {
            continue;
        }
        if (!shown) {
            std::printf("%-16s %-8s %s\n", "Name", "Type", "Status");
            shown = true;
        }
        print_row(row);
    }
    if (device != nullptr && !shown) {
        std::fprintf(stderr, "info: no volume named %s\n", device);
        return 10;
    }
    return 0;
}
