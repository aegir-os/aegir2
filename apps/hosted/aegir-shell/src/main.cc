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
#include <aegir/script/interpreter.h>
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

/* The first word of the rest read as a decimal, for Quit and FailAt. */
bool parse_number(std::string const &text, uint64_t &out)
{
    std::vector<std::string> const words = split_words(text);
    if (words.empty()) {
        return false;
    }
    uint64_t value = 0;
    for (char const c : words[0]) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    out = value;
    return true;
}

/* A line's redirections, pulled off before the command: the Amiga's `>` a
 * file created and cut, `>>` a file appended, `<` a file read as input. The
 * operator and its name are one token (`>file`) or two (`> file`), and the
 * words that remain are the command's own (specs/shell.md). */
struct Redirect {
    std::vector<std::string> words;
    std::string in_path;
    std::string out_path;
    bool append = false;
};

Redirect split_redirect(std::vector<std::string> const &raw)
{
    Redirect result;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        std::string const &token = raw[i];
        if (!token.empty() && (token[0] == '>' || token[0] == '<')) {
            bool const input = token[0] == '<';
            std::size_t const operator_length =
                (token.size() > 1 && token[1] == '>') ? 2 : 1;
            bool const append = operator_length == 2;
            std::string target = token.substr(operator_length);
            if (target.empty() && i + 1 < raw.size()) {
                target = raw[++i];
            }
            if (input) {
                result.in_path = target;
            } else {
                result.out_path = target;
                result.append = append;
            }
            continue;
        }
        result.words.push_back(token);
    }
    return result;
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

/* Starting a command file: it began, it is not there, or it is already
 * running (a cycle, refused rather than recursed). */
enum class ScriptStart { Started, NotFound, Cycle };

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
        print("Aegir shell -- CD, Echo, Set/Get, Alias, Prompt, Why, Eval, "
              "Execute, Quit, FailAt, EndCLI\n");
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

    /* Start a command file: read it through the session's namespace and push
     * its executable lines as a frame for the loop to drain. */
    ScriptStart start_script(std::string const &path)
    {
        std::FILE *const file = std::fopen(path.c_str(), "rb");
        if (file == nullptr) {
            return ScriptStart::NotFound;
        }
        std::string text;
        char chunk[512];
        std::size_t have = 0;
        while ((have = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
            text.append(chunk, have);
        }
        std::fclose(file);
        if (!frames_.push(path, aegir::script::script_lines(text))) {
            return ScriptStart::Cycle;
        }
        return ScriptStart::Started;
    }

    /* Shell-Startup (specs/shell.md): the shell's own startup, the user's
     * Home:S file first and the system's under it. Run once, before the loop
     * reads the console; the first that reads is the one that runs, and a
     * session with neither just starts. */
    void run_startup()
    {
        static char const *const kStartup[] = {"S:Shell-Startup",
                                               "Sys:S/Shell-Startup"};
        for (char const *candidate : kStartup) {
            if (start_script(candidate) == ScriptStart::Started) {
                return;
            }
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
                    /* FailAt (specs/shell.md): a return code at or above the
                     * level aborts the running command file. */
                    if (!frames_.empty() &&
                        aegir::script::Frames::fails(status, frames_.fail_level())) {
                        frames_.abort();
                    }
                }
            }
            /* A running command file's next line comes before the console: a
             * spawned command completes mid-script, and the line after it must
             * run rather than the prompt. Begin the next prompt whenever no
             * command runs: a read_line with no line ready begins the editor
             * and returns empty, which is how the prompt is drawn after a
             * command's exit. */
            if (!busy_) {
                std::string const *line = frames_.next();
                if (line != nullptr) {
                    answered = true;
                    bool const spawned = run_line(*line);
                    if (!spawned && !frames_.empty() &&
                        aegir::script::Frames::fails(line_status_, frames_.fail_level())) {
                        frames_.abort();
                    }
                } else {
                    /* No command file left: the boot script is done, and auth
                     * is waiting on the notification (specs/shell.md). */
                    boot_done();
                    char buffer[1024];
                    uint32_t const have =
                        aegir::console::stream_read_line(port_, buffer, sizeof(buffer));
                    if (have > 0) {
                        answered = true;
                        (void)run_line(std::string(buffer, have));
                    }
                }
            }
            if (!answered) {
                seL4_Wait(doorbell_, nullptr);
            }
        }
    }

    void set_doorbell(seL4_CPtr doorbell) { doorbell_ = doorbell; }

    /* The boot session signals auth when its command file is done, after which
     * auth starts the greeter (specs/shell.md, specs/boot.md). The notification
     * is the boot terminal's grant, passed on by name; an interactive shell has
     * none. */
    void set_boot_notification()
    {
        uint64_t slot = 0;
        if (aegir::bootstrap::capability("boot.doorbell", 13, &slot)) {
            boot_notification_ = static_cast<seL4_CPtr>(slot);
        }
    }

    /* Run the command file the shell was started with -- the boot session's
     * Startup-Sequence. It is a frame like any other, and the loop drains it;
     * the boot notification is signalled when it empties or EndCLI takes it. */
    void run_boot_script(std::string const &path)
    {
        if (start_script(path) == ScriptStart::Started) {
            booting_ = true;
        }
    }

private:
    /* The boot script has finished: wake auth once, and only once. */
    void boot_done()
    {
        if (!booting_) {
            return;
        }
        booting_ = false;
        if (boot_notification_ != 0) {
            seL4_Signal(boot_notification_);
        }
    }

    void print(std::string const &text)
    {
        /* A built-in's output goes to the redirection when the line gave one
         * (specs/shell.md): `EndCLI >NIL:` closes without the line showing. */
        if (redirect_out_ != nullptr) {
            (void)std::fwrite(text.data(), 1, text.size(), redirect_out_);
            return;
        }
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
        /* A boot script that ends with EndCLI closes before the frame could
         * empty, so the notification goes now (specs/boot.md). */
        boot_done();
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

    /* Eval runs a line: it is a one-line command file, pushed as a frame so a
     * program it names completes mid-line the way a script's line does. */
    void command_eval(std::string const &arg)
    {
        if (arg.empty()) {
            print("Eval: what line?\n");
            line_status_ = 5;
            return;
        }
        (void)frames_.push(std::string{}, aegir::script::script_lines(arg));
    }

    /* Execute runs a command file (specs/shell.md). The file is read through
     * the session's namespace, its executable lines become a frame, and the
     * loop takes them from there; a file already running is a cycle and is
     * refused rather than recursed. */
    void command_execute(std::string const &arg)
    {
        std::vector<std::string> const words = split_words(arg);
        if (words.empty()) {
            print("Execute: which command file?\n");
            line_status_ = 10;
            return;
        }
        std::string const path = resolve(words[0]);
        switch (start_script(path)) {
        case ScriptStart::Started:
            return;
        case ScriptStart::Cycle:
            print("Execute: " + words[0] + ": already running\n");
            break;
        case ScriptStart::NotFound:
            print("Execute: " + words[0] + ": not found\n");
            break;
        }
        line_status_ = 10;
    }

    /* Quit ends the current command file with the return code it is given
     * (specs/shell.md's interpreter). From the prompt it says so rather than
     * ending the session -- EndCLI is the session's end. */
    void command_quit(std::string const &arg)
    {
        uint64_t code = 0;
        if (!arg.empty() && !parse_number(arg, code)) {
            print("Quit: a return code, please\n");
            line_status_ = 5;
            return;
        }
        if (!frames_.abort()) {
            print("Quit: no command file is running\n");
            line_status_ = 5;
            return;
        }
        line_status_ = code;
    }

    /* FailAt sets the level at or above which a return code aborts a running
     * command file; the Amiga's default is 10. No argument reports it. */
    void command_failat(std::string const &arg)
    {
        if (arg.empty()) {
            print("FailAt " + std::to_string(frames_.fail_level()) + "\n");
            return;
        }
        uint64_t level = 0;
        if (!parse_number(arg, level)) {
            print("FailAt: a return code level, please\n");
            line_status_ = 5;
            return;
        }
        frames_.set_fail_level(level);
    }

    /* Run one line: true when it started a program (busy_ is set and the exit
     * comes later), false when it finished here (a built-in, a directory
     * change, an unknown name). line_status_ is the line's own return code,
     * which the fail level checks for a built-in. */
    bool run_line(std::string const &line)
    {
        line_status_ = 0;
        /* An alias stands for a line, and the Amiga expands the first word
         * again, so an alias may name another (specs/dos.md). A name seen
         * twice stops the walk, which is a cycle, not a depth limit. */
        std::string const expanded = expand_aliases(line);
        Redirect const redirect = split_redirect(split_words(expanded));
        std::vector<std::string> const &words = redirect.words;
        if (words.empty()) {
            return false;
        }
        std::string const command = to_lower(words[0]);
        std::string arg;
        for (std::size_t i = 1; i < words.size(); ++i) {
            if (i > 1) {
                arg.push_back(' ');
            }
            arg += words[i];
        }

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
            {"execute", &Shell::command_execute},
            {"quit", &Shell::command_quit},
            {"failat", &Shell::command_failat},
            {"endcli", &Shell::command_endcli},
            {"endshell", &Shell::command_endcli},
        };
        for (Builtin const &builtin : kBuiltins) {
            if (command == builtin.name) {
                /* A built-in's output is the shell's own, so its redirection is
                 * the shell's too: open the target and print into it. */
                if (!redirect.out_path.empty()) {
                    std::FILE *const file = std::fopen(
                        redirect.out_path.c_str(), redirect.append ? "ab" : "wb");
                    if (file == nullptr) {
                        print("Cannot open " + redirect.out_path + "\n");
                        line_status_ = 10;
                        return false;
                    }
                    redirect_out_ = file;
                    (this->*builtin.handler)(arg);
                    redirect_out_ = nullptr;
                    std::fclose(file);
                } else {
                    (this->*builtin.handler)(arg);
                }
                return false;
            }
        }

        /* Not a built-in: a directory typed on its own is the Amiga's implicit
         * change into it (specs/shell.md); anything else is a program. */
        if (is_directory(resolve(words[0]))) {
            (void)change_directory(words[0]);
            return false;
        }
        /* A program, run by the terminal on this shell's behalf: it owns the
         * spawn authority and starts the command with this stream. The command
         * line is the command token, lowercased (the Amiga is case-blind and
         * C: is not, specs/dos.md), then its arguments; the redirections
         * already left it. */
        std::string command_line = command;
        for (std::size_t i = 1; i < words.size(); ++i) {
            command_line.push_back(' ');
            command_line += words[i];
        }
        std::string const cwd = current_directory();
        std::string const environment = environment_string();
        if (aegir::console::stream_run(
                port_, command_line.data(), static_cast<uint32_t>(command_line.size()),
                cwd.c_str(), static_cast<uint32_t>(cwd.size()), environment.data(),
                static_cast<uint32_t>(environment.size()), redirect.in_path.data(),
                static_cast<uint32_t>(redirect.in_path.size()), redirect.out_path.data(),
                static_cast<uint32_t>(redirect.out_path.size()))) {
            busy_ = true;
            return true;
        }
        print("Unknown command: " + words[0] + "\n");
        line_status_ = 10;
        return false;
    }

    aegir::ipc::Consumer port_;
    seL4_CPtr doorbell_ = 0;
    bool busy_ = false;
    aegir::script::Frames frames_;
    std::vector<std::pair<std::string, std::string>> aliases_;
    std::string prompt_override_;
    uint64_t last_status_ = 0;
    uint64_t line_status_ = 0;
    /* The sink a built-in's output uses while a redirected line runs, or null
     * for the console stream (specs/shell.md). */
    std::FILE *redirect_out_ = nullptr;
    /* The boot session's notification, signalled when Startup-Sequence is done
     * (specs/boot.md); zero for a shell that is not the boot shell. */
    seL4_CPtr boot_notification_ = 0;
    bool booting_ = false;
};

}  // namespace

int main(int argc, char **argv)
{
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
    shell.set_boot_notification();
    shell.load_environment();
    shell.start();
    /* A shell started with a command file is the boot session: it runs
     * Startup-Sequence and signals auth when it is done. A shell started with
     * none is interactive, and runs Shell-Startup (specs/shell.md). */
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
        shell.run_boot_script(argv[1]);
    } else {
        shell.run_startup();
    }
    shell.loop();
    return 0;
}