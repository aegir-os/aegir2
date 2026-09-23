/*
 * The process environment -- implementation. See include/aegir/environment.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/environment.h>

#include <aegir/bootstrap.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

/* sel4runtime's accessors, declared rather than included: its header is C-only
 * (sel4runtime/stdint.h uses _Static_assert), the boundary specs/userland.md
 * records. The frame they read is the one the spawner laid out
 * (aegir-spawn's build_start_frame). */
extern "C" {
int sel4runtime_argc(void);
char const *const *sel4runtime_argv(void);
char const *const *sel4runtime_envp(void);
}

namespace aegir::environment {

namespace {

/* The process's own settings, laid over what it inherited: `setenv` and
 * `set_current_dir` write here, and the frame and the block stay read-only. */
struct State {
    std::vector<std::pair<std::string, std::string>> settings;
    std::string cwd;
    bool cwd_set = false;
};

State &state()
{
    static State values;
    return values;
}

}  // namespace

int argc() noexcept
{
    return sel4runtime_argc();
}

char const *const *argv() noexcept
{
    return sel4runtime_argv();
}

char const *getenv(char const *name) noexcept
{
    if (name == nullptr) {
        return nullptr;
    }
    for (auto const &setting : state().settings) {
        if (setting.first == name) {
            return setting.second.c_str();
        }
    }
    char const *const *envp = sel4runtime_envp();
    if (envp == nullptr) {
        return nullptr;
    }
    size_t const length = std::strlen(name);
    for (uint32_t i = 0; envp[i] != nullptr; ++i) {
        if (std::strncmp(envp[i], name, length) == 0 && envp[i][length] == '=') {
            return envp[i] + length + 1;
        }
    }
    return nullptr;
}

bool setenv(char const *name, char const *value) noexcept
{
    if (name == nullptr || value == nullptr || name[0] == '\0' ||
        std::strchr(name, '=') != nullptr) {
        return false;
    }
    for (auto &setting : state().settings) {
        if (setting.first == name) {
            setting.second = value;
            return true;
        }
    }
    state().settings.emplace_back(name, value);
    return true;
}

std::string_view current_dir() noexcept
{
    if (state().cwd_set) {
        return state().cwd;
    }
    uint32_t length = 0;
    char const *found = bootstrap::current_dir(&length);
    if (found == nullptr) {
        return {};
    }
    return std::string_view(found, length);
}

bool set_current_dir(std::string_view path) noexcept
{
    state().cwd.assign(path.data(), path.size());
    state().cwd_set = true;
    return true;
}

}  // namespace aegir::environment
