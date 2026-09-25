/*
 * assign: bind a namespace alias (specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * An alias in the session's namespace: the name stands for a path, and a
 * later command resolves it. The binding is the caller's own, so the command
 * uses the namespace's self forms -- it cannot read the badge on the
 * capability it holds (specs/namespace.md). ADD appends a member to a union,
 * the default replaces, and REMOVE drops one name.
 */

#include <aegir/args.h>
#include <aegir/command.h>
#include <aegir/nmspace.h>
#include <aegir/vfs.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv)
{
    if (!aegir::command::start("assign")) {
        std::_Exit(127);
    }
    aegir::args::Result const args =
        aegir::args::read("NAME,TARGET,ADD/S,REMOVE/S", argc - 1, argv + 1);
    if (!args.ok()) {
        std::fprintf(stderr, "assign: %.*s\nusage: %s\n",
                     static_cast<int>(args.missing_length), args.missing, args.usage());
        return 10;
    }
    char const *const name = args.value("NAME");
    char const *const target = args.value("TARGET");
    bool const remove = args.present("REMOVE");
    bool const add = args.present("ADD");

    if (remove) {
        if (name == nullptr) {
            std::fprintf(stderr, "assign: REMOVE needs a name\nusage: %s\n", args.usage());
            return 10;
        }
        aegir::vfs::Namespace space = aegir::vfs::Namespace::find();
        if (!space.valid()) {
            std::fprintf(stderr, "assign: no namespace\n");
            return 10;
        }
        if (!space.unbind(name, static_cast<uint32_t>(std::strlen(name)))) {
            std::fprintf(stderr, "assign: %s is not assigned\n", name);
            return 5;
        }
        return 0;
    }
    if (target == nullptr || name == nullptr) {
        std::fprintf(stderr, "assign: NAME and TARGET are both needed\nusage: %s\n",
                     args.usage());
        return 10;
    }
    aegir::vfs::Namespace space = aegir::vfs::Namespace::find();
    if (!space.valid()) {
        std::fprintf(stderr, "assign: no namespace\n");
        return 10;
    }
    uint64_t const flags = add ? aegir::nmspace::kBindAppend : 0;
    if (!space.bind(name, static_cast<uint32_t>(std::strlen(name)), target,
                    static_cast<uint32_t>(std::strlen(target)), flags)) {
        std::fprintf(stderr, "assign: cannot assign %s to %s\n", name, target);
        return 10;
    }
    return 0;
}
