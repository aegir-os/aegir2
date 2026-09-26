/*
 * aegir-shell: the Amiga command line, as its own process (specs/shell.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The shell is a CON: client: it opens a cooked stream on the terminal's
 * con.stream, prints a prompt, reads lines, and either does something itself
 * -- CD, Echo, Set/Get, Alias, Prompt, Why, Eval, EndCLI/EndShell -- or asks
 * the terminal to run a command. The terminal owns
 * the window and the spawn authority; the shell owns the loop and the words.
 * Its doorbell is a notification it passes on
 * open: the terminal rings it when a line is ready or a command has finished,
 * so the shell waits instead of polling (specs/terminal.md).
 */

#include <aegir/bootstrap.h>
#include <aegir/console_stream.h>
#include <aegir/console_stream_client.h>
#include <aegir/debug.h>
#include <aegir/environment.h>
#include <aegir/heap.h>
#include <aegir/ipc/port.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <sel4/sel4.h>

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

/* Static, like every hosted smoke's: the allocator's tables are tens of
 * kilobytes and a service's stack is pages. The node pool is what lets a
 * 22-bit untyped split all the way down to a notification. */
aegir::mem::Allocator g_objects(nullptr);
aegir::mem::Scratch g_scratch(nullptr);
alignas(64) unsigned char g_nodes[64 * 1024];

bool adopt_memory()
{
    uint64_t untyped_slot = 0;
    uint64_t vspace_slot = 0;
    uint64_t window_base = 0;
    uint32_t window_bytes = 0;
    uint64_t untyped_physical = 0;
    uint32_t untyped_bits = 0;
    uint64_t untyped_address = 0;
    static_cast<void>(aegir::bootstrap::untyped(&untyped_physical, &untyped_bits,
                                                &untyped_address));

    uint64_t first_free = aegir::bootstrap::kSlotFirstDeclared;
    aegir::bootstrap::Block const *block = aegir::bootstrap::find();
    if (block != nullptr) {
        for (uint32_t e = 0; e < block->entry_count; ++e) {
            aegir::bootstrap::Entry const &entry = block->entries[e];
            if (entry.kind == aegir::bootstrap::EntryKind::Capability &&
                entry.number + 1 > first_free) {
                first_free = entry.number + 1;
            }
        }
    }

    bool ok = aegir::bootstrap::capability("untyped", 7, &untyped_slot) &&
              aegir::bootstrap::capability("vspace", 6, &vspace_slot) &&
              aegir::bootstrap::window(&window_base, &window_bytes) &&
              g_objects.adopt_untyped(static_cast<seL4_CPtr>(untyped_slot), untyped_bits,
                                      untyped_physical);
    if (ok) {
        g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::kCNodeBits) - first_free, 0);
        ok = g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                             static_cast<uintptr_t>(window_base),
                             static_cast<uintptr_t>(window_base + window_bytes), &g_objects);
    }
    return ok;
}

std::string to_lower(std::string text)
{
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::vector<std::string> split_words(std::string const &line)
{
    std::vector<std::string> words;
    std::string current;
    for (char const c : line) {
        if (c == ' ' || c == '\t') {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        words.push_back(current);
    }
    return words;
}

std::string join_tail(std::string const &line, std::string const &first)
{
    std::size_t const at = line.find(first);
    if (at == std::string::npos) {
        return {};
    }
    std::size_t i = at + first.size();
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    return line.substr(i);
}

/* The line with its command token lowercased: the Amiga is case-blind, C: is
 * not, and the command a typed `Copy` names is C:copy (specs/dos.md). */
std::string with_lowercased_command(std::string const &line, std::string const &command)
{
    std::size_t const at = line.find(command);
    if (at == std::string::npos) {
        return to_lower(line);
    }
    return line.substr(0, at) + to_lower(command) + line.substr(at + command.size());
}

std::string parent_of(std::string const &path)
{
    if (path.empty() || path.back() == ':') {
        return path;
    }
    std::size_t const slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        std::size_t const colon = path.find(':');
        return colon == std::string::npos ? path : path.substr(0, colon + 1);
    }
    return path.substr(0, slash);
}

std::string append_component(std::string const &base, std::string const &component)
{
    if (base.empty() || base.back() == ':') {
        return base + component;
    }
    return base + "/" + component;
}

std::string current_directory()
{
    std::error_code error;
    std::filesystem::path const path = std::filesystem::current_path(error);
    return error ? std::string("?") : path.string();
}

std::string prompt_for(std::string const &directory)
{
    return directory + ">";
}

std::string environment_string()
{
    std::string out;
    char const *const *entries = aegir::environment::environ();
    for (uint32_t i = 0; entries[i] != nullptr; ++i) {
        out.append(entries[i]);
        out.push_back('\0');
    }
    return out;
}

/* A built-in: the shell's own words, which it runs itself because they touch
 * its state (the current directory, the environment, the aliases, the prompt)
 * or are part of the command language. The table is the dispatch; a name not in
 * it is a program, resolved from C: and started by the terminal (specs/dos.md).
 * Several names share a handler, which is the Amiga's synonyms. */
class Shell;
struct Builtin {
    char const *name;
    void (Shell::*handler)(std::string const &);
};

class Shell {
public:
    explicit Shell(aegir::ipc::Consumer port) : port_(port) {}

    void start()
    {
        std::string const prompt = prompt_for(current_directory());
        if (!aegir::console::stream_open(port_, aegir::console::kStreamModeCooked,
                                         prompt.c_str(),
                                         static_cast<uint32_t>(prompt.size()), doorbell_)) {
            aegir::debug_write("  aegir-shell: FAIL the stream would not open\n");
            std::exit(1);
        }
        print("Aegir shell -- CD, Echo, Set/Get, Alias, Prompt, Why, Eval, EndCLI\n");
    }

    /* The persistent environment (specs/environment.md): the merged ENV: view
     * -- the user's archive first, the system's base under it -- read once at
     * startup, each file a NAME=VALUE this process then carries. A write makes
     * a new name in the create target, the user's archive. */
    void load_environment()
    {
        std::error_code error;
        std::filesystem::directory_iterator it("ENV:", error);
        if (error) {
            return;
        }
        std::filesystem::directory_iterator const end;
        for (; !error && it != end; it.increment(error)) {
            std::error_code kind_error;
            if (it->is_directory(kind_error)) {
                continue;
            }
            std::string const name = it->path().filename().string();
            std::string const path = "ENV:" + name;
            std::FILE *const file = std::fopen(path.c_str(), "rb");
            if (file == nullptr) {
                continue;
            }
            std::string value;
            char chunk[256];
            std::size_t have = 0;
            while ((have = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
                value.append(chunk, have);
            }
            std::fclose(file);
            (void)aegir::environment::setenv(name.c_str(), value.c_str());
        }
    }

    void loop()
    {
        for (;;) {
            bool answered = false;
            if (busy_) {
                uint64_t status = 0;
                if (aegir::console::stream_command_status(port_, &status)) {
                    busy_ = false;
                    answered = true;
                    last_status_ = status;
                    if (status != 0) {
                        print("return code " + std::to_string(status) + "\n");
                    }
                }
            }
            /* Begin the next prompt whenever no command runs: a read_line with
             * no line ready begins the editor and returns empty, which is how
             * the prompt is drawn after a command's exit. */
            if (!busy_) {
                char line[1024];
                uint32_t const have =
                    aegir::console::stream_read_line(port_, line, sizeof(line));
                if (have > 0) {
                    answered = true;
                    run_line(std::string(line, have));
                }
            }
            if (!answered) {
                seL4_Wait(doorbell_, nullptr);
            }
        }
    }

    void set_doorbell(seL4_CPtr doorbell) { doorbell_ = doorbell; }

private:
    void print(std::string const &text)
    {
        (void)aegir::console::stream_write(port_, text.data(),
                                           static_cast<uint32_t>(text.size()));
    }

    void refresh_prompt()
    {
        std::string const prompt =
            prompt_override_.empty() ? prompt_for(current_directory()) : prompt_override_;
        (void)aegir::console::stream_set_prompt(port_, prompt.c_str(),
                                                static_cast<uint32_t>(prompt.size()));
    }

    std::string resolve(std::string const &arg) const
    {
        if (arg.find(':') != std::string::npos) {
            return arg;
        }
        std::string base = current_directory();
        std::size_t i = 0;
        while (i <= arg.size()) {
            std::size_t const slash = arg.find('/', i);
            std::string const component =
                arg.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
            if (component == ".." || component.empty()) {
                base = parent_of(base);
            } else if (component != ".") {
                base = append_component(base, component);
            }
            if (slash == std::string::npos) {
                break;
            }
            i = slash + 1;
        }
        return base;
    }

    bool is_directory(std::string const &path) const
    {
        std::error_code error;
        return std::filesystem::is_directory(path, error) && !error;
    }

    bool change_directory(std::string const &arg)
    {
        std::string const target = resolve(arg);
        if (!is_directory(target)) {
            return false;
        }
        std::error_code error;
        std::filesystem::current_path(target, error);
        if (error) {
            return false;
        }
        refresh_prompt();
        return true;
    }

    void command_cd(std::string const &arg)
    {
        if (arg.empty()) {
            print(current_directory() + "\n");
        } else if (!change_directory(arg)) {
            print("CD: not a directory: " + arg + "\n");
        }
    }

    void command_echo(std::string const &arg)
    {
        print(arg + "\n");
    }

    /* EndCLI and EndShell are the same command -- EndCLI the older spelling,
     * EndShell the newer; both end this shell process (AmigaOS 3.1 reference).
     * Quit is a different thing: it aborts a script with a return code, and
     * belongs to the interpreter arc. */
    void command_endcli(std::string const &arg)
    {
        static_cast<void>(arg);
        print("bye\n");
        (void)aegir::console::stream_close(port_, 0);
        std::exit(0);
    }

    void command_set(std::string const &arg)
    {
        std::size_t const space = arg.find_first_of(" \t");
        std::string const name = space == std::string::npos ? arg : arg.substr(0, space);
        std::string value;
        if (space != std::string::npos) {
            std::size_t const start = arg.find_first_not_of(" \t", space);
            if (start != std::string::npos) {
                value = arg.substr(start);
            }
        }
        if (name.empty() || !aegir::environment::setenv(name.c_str(), value.c_str())) {
            print("Set: a name and a value, please\n");
            return;
        }
        /* Persist it (specs/environment.md): the archive is a directory of
         * files, one per variable, and a create lands in the create target --
         * the user's archive. A file that will not open leaves the variable
         * set for this process only, which the message says. */
        std::string const path = "ENV:" + name;
        std::FILE *const file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) {
            print("Set: " + name + " set, but not saved\n");
            return;
        }
        (void)std::fwrite(value.data(), 1, value.size(), file);
        std::fclose(file);
    }

    void command_get(std::string const &arg)
    {
        std::size_t const space = arg.find_first_of(" \t");
        std::string const name = space == std::string::npos ? arg : arg.substr(0, space);
        if (name.empty()) {
            print("Get: what variable?\n");
            return;
        }
        char const *const value = aegir::environment::getenv(name.c_str());
        print(name + "=" + (value != nullptr ? value : "(not set)") + "\n");
    }

    void command_unset(std::string const &arg)
    {
        std::size_t const space = arg.find_first_of(" \t");
        std::string const name = space == std::string::npos ? arg : arg.substr(0, space);
        if (name.empty()) {
            print("UnSet: what variable?\n");
            return;
        }
        aegir::environment::unsetenv(name.c_str());
        /* The persisted copy is the union's create target; removing it lets an
         * inherited base value show through again (specs/environment.md). */
        std::remove(("ENV:" + name).c_str());
    }

    /* An alias stands for a line, expanded until the first word is no longer
     * one: the Amiga's Alias is recursive, and a name seen twice is a cycle,
     * not a depth the shell chose. */
    std::string expand_aliases(std::string const &line) const
    {
        std::string expanded = line;
        std::vector<std::string> seen;
        for (;;) {
            std::vector<std::string> const words = split_words(expanded);
            if (words.empty()) {
                break;
            }
            std::string const name = to_lower(words[0]);
            bool stop = false;
            for (auto const &already : seen) {
                if (already == name) {
                    stop = true;
                    break;
                }
            }
            if (stop) {
                break;
            }
            std::string const *expansion = nullptr;
            for (auto const &alias : aliases_) {
                if (alias.first == name) {
                    expansion = &alias.second;
                    break;
                }
            }
            if (expansion == nullptr) {
                break;
            }
            seen.push_back(name);
            std::string const tail = join_tail(expanded, words[0]);
            expanded = tail.empty() ? *expansion : *expansion + " " + tail;
        }
        return expanded;
    }

    void set_alias(std::string const &name, std::string const &value)
    {
        for (auto &alias : aliases_) {
            if (alias.first == name) {
                alias.second = value;
                return;
            }
        }
        aliases_.emplace_back(name, value);
    }

    void command_alias(std::string const &arg)
    {
        std::vector<std::string> const words = split_words(arg);
        if (words.empty()) {
            for (auto const &alias : aliases_) {
                print(alias.first + "=" + alias.second + "\n");
            }
            return;
        }
        std::string const name = to_lower(words[0]);
        std::string const tail = join_tail(arg, words[0]);
        if (tail.empty()) {
            for (auto const &alias : aliases_) {
                if (alias.first == name) {
                    print(name + "=" + alias.second + "\n");
                    return;
                }
            }
            print("Alias: " + name + " is not defined\n");
            return;
        }
        set_alias(name, tail);
    }

    void command_unalias(std::string const &arg)
    {
        std::vector<std::string> const words = split_words(arg);
        if (words.empty()) {
            print("UnAlias: what alias?\n");
            return;
        }
        std::string const name = to_lower(words[0]);
        for (auto it = aliases_.begin(); it != aliases_.end(); ++it) {
            if (it->first == name) {
                aliases_.erase(it);
                return;
            }
        }
        print("UnAlias: " + name + " is not defined\n");
    }

    void command_prompt(std::string const &arg)
    {
        /* No argument restores the directory-shaped default (specs/shell.md). */
        prompt_override_ = arg;
        refresh_prompt();
    }

    void command_why(std::string const &arg)
    {
        uint64_t code = last_status_;
        if (!arg.empty()) {
            uint64_t parsed = 0;
            bool digits = true;
            for (char const c : arg) {
                if (c < '0' || c > '9') {
                    digits = false;
                    break;
                }
                parsed = parsed * 10 + static_cast<uint64_t>(c - '0');
            }
            if (!digits) {
                print("Why: a return code, please\n");
                return;
            }
            code = parsed;
        }
        char const *explanation = "a return code";
        if (code == 0) {
            explanation = "no error";
        } else if (code < 10) {
            explanation = "a warning";
        } else if (code < 20) {
            explanation = "an error";
        } else if (code < 30) {
            explanation = "a failure";
        }
        print("Why: " + std::to_string(code) + " (" + explanation + ")\n");
    }

    void command_eval(std::string const &arg)
    {
        if (arg.empty()) {
            print("Eval: what line?\n");
            return;
        }
        run_line(arg);
    }

    void run_line(std::string const &line)
    {
        /* An alias stands for a line, and the Amiga expands the first word
         * again, so an alias may name another (specs/dos.md). A name seen
         * twice stops the walk, which is a cycle, not a depth limit. */
        std::string const expanded = expand_aliases(line);
        std::vector<std::string> const words = split_words(expanded);
        if (words.empty()) {
            return;
        }
        std::string const command = to_lower(words[0]);
        std::string const arg = join_tail(expanded, words[0]);

        static Builtin const kBuiltins[] = {
            {"cd", &Shell::command_cd},
            {"currentdir", &Shell::command_cd},
            {"echo", &Shell::command_echo},
            {"set", &Shell::command_set},
            {"setvar", &Shell::command_set},
            {"setenv", &Shell::command_set},
            {"get", &Shell::command_get},
            {"getvar", &Shell::command_get},
            {"getenv", &Shell::command_get},
            {"unset", &Shell::command_unset},
            {"unsetenv", &Shell::command_unset},
            {"alias", &Shell::command_alias},
            {"unalias", &Shell::command_unalias},
            {"prompt", &Shell::command_prompt},
            {"why", &Shell::command_why},
            {"fault", &Shell::command_why},
            {"eval", &Shell::command_eval},
            {"endcli", &Shell::command_endcli},
            {"endshell", &Shell::command_endcli},
        };
        for (Builtin const &builtin : kBuiltins) {
            if (command == builtin.name) {
                (this->*builtin.handler)(arg);
                return;
            }
        }

        /* Not a built-in: a directory typed on its own is the Amiga's implicit
         * change into it (specs/shell.md); anything else is a program. */
        if (is_directory(resolve(words[0]))) {
            (void)change_directory(words[0]);
            return;
        }
        /* A program, run by the terminal on this shell's behalf: it owns the
         * spawn authority and starts the command with this stream. The command
         * token is lowercased first -- the Amiga is case-blind and C: is not
         * (specs/dos.md) -- so `Copy` resolves C:copy. */
        std::string const command_line = with_lowercased_command(expanded, words[0]);
        std::string const cwd = current_directory();
        std::string const environment = environment_string();
        if (aegir::console::stream_run(port_, command_line.data(),
                                       static_cast<uint32_t>(command_line.size()),
                                       cwd.c_str(), static_cast<uint32_t>(cwd.size()),
                                       environment.data(),
                                       static_cast<uint32_t>(environment.size()))) {
            busy_ = true;
            return;
        }
        print("Unknown command: " + words[0] + "\n");
    }

    aegir::ipc::Consumer port_;
    seL4_CPtr doorbell_ = 0;
    bool busy_ = false;
    std::vector<std::pair<std::string, std::string>> aliases_;
    std::string prompt_override_;
    uint64_t last_status_ = 0;
};

}  // namespace

int main(int argc, char **argv)
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    g_objects.adopt_nodes(g_nodes, sizeof(g_nodes));
    if (!adopt_memory()) {
        aegir::debug_write("  aegir-shell: FAIL no untyped, vspace or window\n");
        std::_Exit(127);
    }
    constexpr uint64_t kHeapBytes = 8ull << 20;
    if (!aegir::heap::init(g_objects, g_scratch, kHeapBytes)) {
        aegir::debug_write("  aegir-shell: FAIL the heap would not claim the window\n");
        std::_Exit(127);
    }

    aegir::ipc::Consumer const port = aegir::ipc::Consumer::find(
        aegir::console::kStreamPortName, aegir::console::kStreamPortNameLength);
    if (!port.valid()) {
        aegir::debug_write("  aegir-shell: FAIL no con.stream\n");
        std::_Exit(127);
    }

    /* The doorbell: the terminal rings it when a line is ready or a command
     * has finished, so the shell waits instead of polling. */
    seL4_Error error = seL4_NoError;
    aegir::mem::Account account{"shell", 0, 0, 0};
    seL4_CPtr const doorbell = g_objects.alloc_object(seL4_NotificationObject,
                                                      seL4_NotificationBits, account, &error);
    if (doorbell == 0) {
        aegir::debug_write("  aegir-shell: FAIL no doorbell notification\n");
        std::_Exit(127);
    }

    Shell shell(port);
    shell.set_doorbell(doorbell);
    shell.load_environment();
    shell.start();
    shell.loop();
    return 0;
}