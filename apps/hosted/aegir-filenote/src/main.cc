/*
 * filenote: attach a comment to a file (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The comment is a BFS attribute, AEGIR:COMMENT, reached through the POSIX
 * xattr calls the runtime maps onto the metadata protocol. With no COMMENT the
 * note is removed (an absent attribute is no comment). A filesystem with no
 * attributes -- FAT -- answers EOPNOTSUPP, and the message names the volume's
 * filesystem, which the namespace's describe_path supplies.
 */

#include <aegir/args.h>
#include <aegir/command.h>
#include <aegir/metadata.h>
#include <aegir/vfs.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/xattr.h>

namespace {

/* The volume's filesystem type, for the message a filesystem without
 * attributes earns. Empty when it cannot be learned. */
void filesystem_type(char const *file, char *out, uint32_t capacity) noexcept
{
    out[0] = '\0';
    aegir::vfs::Namespace const space = aegir::vfs::Namespace::find();
    aegir::nmspace::Row row{};
    if (!space.valid() ||
        !space.describe_path(file, static_cast<uint32_t>(std::strlen(file)), row)) {
        return;
    }
    uint32_t i = 0;
    while (i + 1 < capacity && row.type[i] != '\0') {
        out[i] = row.type[i];
        ++i;
    }
    out[i] = '\0';
}

}  // namespace

int main(int argc, char **argv)
{
    if (!aegir::command::start("filenote")) {
        std::_Exit(127);
    }
    aegir::args::Result const args =
        aegir::args::read("FILE/A,COMMENT", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "filenote: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    char const *const file = args.value("FILE");
    char const *const comment = args.value("COMMENT");
    int result = 0;
    if (comment == nullptr) {
        result = ::removexattr(file, aegir::metadata::kNameComment);
        if (result != 0 && errno == ENODATA) {
            result = 0; /* no note to remove is the state that was asked for */
        }
    } else {
        result = ::setxattr(file, aegir::metadata::kNameComment, comment,
                            std::strlen(comment), 0);
    }
    if (result != 0) {
        if (errno == EOPNOTSUPP) {
            char type[32];
            filesystem_type(file, type, sizeof(type));
            if (type[0] != '\0') {
                std::fprintf(stderr, "filenote: %s does not support attributes\n", type);
            } else {
                std::fprintf(stderr,
                             "filenote: the filesystem does not support attributes\n");
            }
        } else {
            std::fprintf(stderr, "filenote: cannot set a comment on %s\n", file);
        }
        return 10;
    }
    return 0;
}
