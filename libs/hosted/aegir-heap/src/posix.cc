/*
 * aegir-heap: the POSIX spawn entry point (specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The runtime the program links provides `posix_spawn`, so a plain C program
 * calls it as on any system. musl's own is clone+execve, neither of which Aegir
 * has; this definition takes its place for a statically linked Aegir program.
 * It builds a launch request from the argv and returns a pid the runtime maps
 * to the child's badge.
 *
 * This file is separate from heap.cc so the launch client's C++ face is not
 * pulled into the dispatcher's translation unit. It reaches the launcher only
 * through the C primitive `aegir_launch_command`, so it includes no libc++
 * header -- and no sel4 header, which would clash with musl's `strcpy`.
 */

#include <aegir/launch.h>

#include <errno.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "posix.h"

namespace aegir::heap {

namespace {

/* The pid table: POSIX's pid_t is 32 bits, an Aegir badge is 64, so a spawned
 * child is given a small pid and its badge remembered here. Grows on demand. */
struct Child {
    int pid;
    uint64_t badge;
};
Child *g_children = nullptr;
uint32_t g_child_count = 0;
uint32_t g_child_capacity = 0;
int g_next_child_pid = 1;

}  // namespace

int register_child(uint64_t badge) noexcept
{
    if (g_child_count == g_child_capacity) {
        uint32_t const grown = g_child_capacity == 0 ? 8 : g_child_capacity * 2;
        Child *const moved = static_cast<Child *>(realloc(g_children, grown * sizeof(Child)));
        if (moved == nullptr) {
            return 0;
        }
        g_children = moved;
        g_child_capacity = grown;
    }
    int const pid = g_next_child_pid++;
    g_children[g_child_count++] = Child{pid, badge};
    return pid;
}

bool take_child_badge(int pid, uint64_t *badge) noexcept
{
    for (uint32_t i = 0; i < g_child_count; ++i) {
        if (g_children[i].pid == pid) {
            *badge = g_children[i].badge;
            g_children[i] = g_children[--g_child_count];
            return true;
        }
    }
    return false;
}

}  // namespace aegir::heap

extern "C" int posix_spawn(pid_t *pid, char const *path,
                           posix_spawn_file_actions_t const *file_actions,
                           posix_spawnattr_t const *attrp, char *const argv[],
                           char *const envp[])
{
    static_cast<void>(file_actions);
    static_cast<void>(attrp);
    static_cast<void>(envp);
    if (pid == nullptr) {
        return EINVAL;
    }
    /* The program words, NUL-separated, program first. The launcher resolves
     * the program from argv[0]; POSIX's `path` is used when no argv was given. */
    char const *const *words = argv;
    char const *single[2] = {path, nullptr};
    if (words == nullptr || words[0] == nullptr) {
        if (path == nullptr) {
            return EINVAL;
        }
        words = single;
    }
    uint32_t total = 0;
    for (uint32_t i = 0; words[i] != nullptr; ++i) {
        total += static_cast<uint32_t>(strlen(words[i])) + 1;
    }
    char *const line = static_cast<char *>(malloc(total != 0 ? total : 1));
    if (line == nullptr) {
        return ENOMEM;
    }
    uint32_t at = 0;
    for (uint32_t i = 0; words[i] != nullptr; ++i) {
        if (i != 0) {
            line[at++] = '\0';
        }
        uint32_t const length = static_cast<uint32_t>(strlen(words[i]));
        memcpy(line + at, words[i], length);
        at += length;
    }
    uint64_t badge = 0;
    int const refused = aegir::launch::aegir_launch_command(line, at, &badge);
    free(line);
    if (refused != 0) {
        return EIO;
    }
    int const child = aegir::heap::register_child(badge);
    if (child == 0) {
        return EAGAIN;
    }
    *pid = child;
    return 0;
}
