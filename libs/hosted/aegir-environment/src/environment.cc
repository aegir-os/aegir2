/*
 * The process environment -- implementation. See include/aegir/environment.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/environment.h>

#include <cstring>
#include <cstdint>
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

/* The current directory lives in the runtime, because chdir/getcwd and
 * std::filesystem::current_path reach the same state (specs/environment.md):
 * this library is the C++ face of it, not a second copy. aegir-heap owns the
 * buffer and answers the two calls. */
extern "C" {
char const *aegir_heap_current_dir(uint32_t *length) noexcept;
int aegir_heap_set_current_dir(char const *path, uint32_t length) noexcept;
}

namespace aegir::environment {

namespace {

/* The process's own settings, laid over what it inherited: `setenv` writes
 * here, and the frame and the block stay read-only. The current directory is
 * the runtime's, not this state's. */
struct State {
    std::vector<std::pair<std::string, std::string>> settings;
    /* The merged view `environ()` hands out: the strings and the pointer array
     * over them, rebuilt each call so the pointers stay valid. */
    std::vector<std::string> merged;
    std::vector<char const *> pointers;
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

char const *const *environ() noexcept
{
    State &s = state();
    s.merged.clear();
    s.pointers.clear();
    char const *const *envp = sel4runtime_envp();
    if (envp != nullptr) {
        for (uint32_t i = 0; envp[i] != nullptr; ++i) {
            size_t name_length = 0;
            while (envp[i][name_length] != '\0' && envp[i][name_length] != '=') {
                ++name_length;
            }
            bool shadowed = false;
            for (auto const &setting : s.settings) {
                if (setting.first.size() == name_length &&
                    std::memcmp(setting.first.c_str(), envp[i], name_length) == 0) {
                    shadowed = true;
                    break;
                }
            }
            if (!shadowed) {
                s.merged.emplace_back(envp[i]);
            }
        }
    }
    for (auto const &setting : s.settings) {
        s.merged.push_back(setting.first + "=" + setting.second);
    }
    s.pointers.reserve(s.merged.size() + 1);
    for (auto const &entry : s.merged) {
        s.pointers.push_back(entry.c_str());
    }
    s.pointers.push_back(nullptr);
    return s.pointers.data();
}

std::string_view current_dir() noexcept
{
    uint32_t length = 0;
    char const *found = aegir_heap_current_dir(&length);
    if (found == nullptr) {
        return {};
    }
    return std::string_view(found, length);
}

bool set_current_dir(std::string_view path) noexcept
{
    return aegir_heap_set_current_dir(path.data(), static_cast<uint32_t>(path.size())) == 0;
}

}  // namespace aegir::environment
