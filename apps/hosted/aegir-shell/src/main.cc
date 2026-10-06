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
 * Its reads wait by holding their reply -- a line, and a running command's
 * status -- so it blocks instead of polling (specs/signal.md).
 */

#include <aegir/bootstrap.h>
#include <aegir/console_stream.h>
#include <aegir/console_stream_client.h>
#include <aegir/debug.h>
#include <aegir/environment.h>
#include <aegir/heap.h>
#include <aegir/ipc/port.h>
#include <aegir/launch_client.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>
#include <aegir/script/condition.h>
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
        g_objects.adopt_slots(first_free, (1u << aegir::bootstrap::kCNodeBits) - first_free, 0,
                          aegir::bootstrap::kCNodeBits);
        ok = g_scratch.adopt(static_cast<seL4_CPtr>(vspace_slot),
                             static_cast<uintptr_t>(window_base),
                             static_cast<uintptr_t>(window_base + window_bytes), &g_objects);
    }
    return ok;
}

/* The shell's own words -- the built-ins and the `Run`/`Alias` keywords -- are
 * matched case-insensitively, because they are the shell's vocabulary and not
 * files: `EndCLI`, `EndCli` and `endcli` are one built-in, as on the Amiga.
 * Nothing is rewritten or lowercased: a command word or a path is still taken
 * as typed and resolved by the filesystem's own case (specs/dos.md). */
bool equals_ci(std::string const &a, char const *b) noexcept
{
    std::size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        char ca = a[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        char cb = b[i];
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return i == a.size() && b[i] == '\0';
}

/* The words after `from`, joined with a space: a built-in that takes its
 * argument as one string (Echo, Prompt, Set's value) wants this, now that the
 * line arrives already split and quote-grouped (specs/shell.md). */
std::string join_words(std::vector<std::string> const &words, std::size_t from = 0)
{
    std::string out;
    for (std::size_t i = from; i < words.size(); ++i) {
        if (i > from) {
            out.push_back(' ');
        }
        out += words[i];
    }
    return out;
}

/* The first word of `line` and the rest after it, each with its original
 * spelling: the alias expansion and the alias definition both need the raw
 * text, not a substituted and split line (specs/shell.md). No quotes are
 * honoured in the split -- a command or an alias name is not quoted. */
void split_word(std::string const &line, std::string &word, std::string &rest)
{
    std::size_t const start = line.find_first_not_of(" \t");
    if (start == std::string::npos) {
        word.clear();
        rest.clear();
        return;
    }
    std::size_t const end = line.find_first_of(" \t", start);
    if (end == std::string::npos) {
        word = line.substr(start);
        rest.clear();
        return;
    }
    word = line.substr(start, end - start);
    std::size_t const rest_start = line.find_first_not_of(" \t", end);
    rest = rest_start == std::string::npos ? std::string{} : line.substr(rest_start);
}

/* The environment a line's `$name` reads: the process's own settings, which
 * the shell loaded from ENV: at startup (specs/environment.md). */
bool environment_lookup(std::string const &name, std::string &value)
{
    char const *const found = aegir::environment::getenv(name.c_str());
    if (found == nullptr) {
        return false;
    }
    value = found;
    return true;
}

/* The first word of `text` read as a decimal, for Quit and FailAt. */
bool parse_number(std::string const &text, uint64_t &out)
{
    if (text.empty()) {
        return false;
    }
    uint64_t value = 0;
    for (char const c : text) {
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

/* Split a substituted line into pipeline stages on a standalone `|` token
 * (specs/pipe.md). A `|` that was quoted is part of a word and does not
 * split; a standalone `|` always does, so the operator needs whitespace
 * around it and a stage may not be empty. */
std::vector<std::vector<std::string>> split_pipeline(
    std::vector<std::string> const &words)
{
    std::vector<std::vector<std::string>> stages(1);
    for (std::string const &word : words) {
        if (word == "|") {
            stages.emplace_back();
        } else {
            stages.back().push_back(word);
        }
    }
    return stages;
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

/* A built-in: the shell's own words, which it runs itself because they touch
 * its state (the current directory, the environment, the aliases, the prompt)
 * or are part of the command language. The table is the dispatch; a name not in
 * it is a program, resolved from C: and started by the terminal (specs/dos.md).
 * Several names share a handler, which is the Amiga's synonyms. */
class Shell;
struct Builtin {
    char const *name;
    void (Shell::*handler)(std::vector<std::string> const &);
};

/* Starting a command file: it began, it is not there, it is already running
 * (a cycle, refused rather than recursed), or its .KEY declaration does not
 * match the arguments it was run with. */
enum class ScriptStart { Started, NotFound, Cycle, Invalid };

class Shell {
public:
    explicit Shell(aegir::ipc::Consumer port) : port_(port) {}

    void start()
    {
        std::string const prompt = prompt_for(current_directory());
        if (!aegir::console::stream_open(port_, aegir::console::kStreamModeCooked,
                                         prompt.c_str(),
                                         static_cast<uint32_t>(prompt.size()))) {
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
     * its executable lines as a frame for the loop to drain. A first line of
     * `.KEY` declares the names the supplied arguments bind to; `$1..$n` are
     * the raw arguments whether or not there is one (specs/shell.md). */
    ScriptStart start_script(std::string const &path,
                             std::vector<std::string> const &supplied = {})
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
        std::vector<std::string> lines = aegir::script::script_lines(text);
        aegir::script::Arguments arguments;
        arguments.name = path;
        arguments.positional = supplied;
        if (!lines.empty() && aegir::script::is_key_line(lines.front())) {
            std::vector<aegir::script::KeySymbol> keys;
            std::string error;
            if (!aegir::script::parse_key_list(lines.front(), keys, error) ||
                !aegir::script::bind_keys(keys, path, supplied, arguments, error)) {
                print("Execute: " + path + ": " + error + "\n");
                return ScriptStart::Invalid;
            }
            lines.erase(lines.begin());
        }
        if (!frames_.push(path, std::move(lines), std::move(arguments))) {
            return ScriptStart::Cycle;
        }
        return ScriptStart::Started;
    }

    /* Shell-Startup (specs/shell.md): the shell's own startup, the user's
     * Home:S file first and the system's under it. Run once, before the loop
     * reads the console; the first that reads is the one that runs, and a
     * session with neither just starts. */
    void run_startup(bool session)
    {
        static char const *const kStartup[] = {"S:Shell-Startup",
                                               "Sys:S/Shell-Startup"};
        for (char const *candidate : kStartup) {
            if (start_script(candidate) == ScriptStart::Started) {
                break;
            }
        }
        /* The session's additions (specs/session.md): `Home:S/User-Startup`
         * once, by the session's own shell -- the one auth marked `--session`
         * -- and not by a nested shell. The frames are a stack, so the file
         * pushed last drains first: the user's additions run before
         * Shell-Startup, and a session whose file is absent runs only
         * Shell-Startup. */
        if (session && start_script("Home:S/User-Startup") == ScriptStart::Started) {
            aegir::debug_write("  shell: User-Startup\n");
        }
    }

    void loop()
    {
        for (;;) {
            if (busy_) {
                uint64_t status = 0;
                if (aegir::console::stream_command_status(port_, &status)) {
                    busy_ = false;
                    last_status_ = status;
                    if (status == aegir::console::kBreakStatus) {
                        /* A Break aborted it (specs/process.md): the Amiga's
                         * `***BREAK` in place of a return-code line, and a
                         * failing status like any other. The serial line is the
                         * acceptance's cue that the shell rendered it. */
                        aegir::debug_write("  shell: ***BREAK\n");
                        print("***BREAK\n");
                    } else if (status != 0) {
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
                    bool const spawned = run_line(*line);
                    /* A line that finished here is the last return code for
                     * the If condition words (WARN/ERROR/FAIL), the same as a
                     * spawned command's exit is (specs/shell.md). */
                    if (!spawned) {
                        last_status_ = line_status_;
                    }
                    if (!spawned && !frames_.empty() &&
                        aegir::script::Frames::fails(line_status_, frames_.fail_level())) {
                        if (booting_) {
                            boot_failed_ = true;
                        }
                        frames_.abort();
                    }
                } else {
                    /* No command file left: the boot script is done, and auth
                     * is waiting on the status (specs/shell.md). */
                    boot_done();
                    if (boot_view_) {
                        /* The failure view is up and takes no input: idle on a
                         * raw read, which paints no prompt and never comes back
                         * (specs/boot.md, specs/signal.md). */
                        char sink = 0;
                        (void)aegir::console::stream_read(port_, &sink, 1);
                        continue;
                    }
                    char buffer[1024];
                    uint32_t const have =
                        aegir::console::stream_read_line(port_, buffer, sizeof(buffer));
                    if (have > 0) {
                        (void)run_line(std::string(buffer, have));
                    }
                }
            }
        }
    }

    /* The boot session signals auth when its command file is done, after which
     * auth starts the greeter (specs/shell.md, specs/boot.md). The channel is
     * an endpoint, not a notification, because the outcome is a word: 0 when
     * Startup-Sequence finished, nonzero when it failed. An interactive shell
     * has none. */
    void set_boot_status()
    {
        uint64_t slot = 0;
        if (aegir::bootstrap::capability("boot.status", 11, &slot)) {
            boot_status_ = static_cast<seL4_CPtr>(slot);
        }
    }

    /* Run the command file the shell was started with -- the boot session's
     * Startup-Sequence. It is a frame like any other, and the loop drains it;
     * the boot notification is signalled when it empties or EndCLI takes it. */
    void run_boot_script(std::string const &path)
    {
        /* The firmware's boot flags, handed down as AEGIR_BOOTARGS: aegir.fail
         * forces the failure view even though the sequence would succeed, which
         * is how the failure path is exercised (specs/boot.md). */
        char const *const bootargs = aegir::environment::getenv("AEGIR_BOOTARGS");
        if (bootargs != nullptr && std::string(bootargs).find("fail") != std::string::npos) {
            boot_forced_fail_ = true;
        }
        if (start_script(path) == ScriptStart::Started) {
            booting_ = true;
        }
    }

    /* Whether this shell is the boot session's: it was handed the boot status
     * channel (specs/boot.md). A shell started with a command file *and* this
     * channel runs Startup-Sequence; without it, that command file is a
     * `NEWSHELL FROM` startup instead. */
    bool has_boot_status() const { return boot_status_ != 0; }

    /* The `FROM <file>` startup (specs/launch.md): run the named file instead
     * of Shell-Startup. Not the boot path -- it sets no `booting_` and
     * signals no auth. A file that will not open falls back to the default
     * startup, the way a missing Shell-Startup does. */
    void run_startup_file(std::string const &path)
    {
        if (start_script(path) == ScriptStart::Started) {
            return;
        }
        run_startup(false);
    }

private:
    /* The built-in table: the shell's own words, the dispatch. A name not in
     * it is a program, resolved from C: and started by the terminal
     * (specs/dos.md). Several names share a handler, the Amiga's synonyms. A
     * static member so a line that must know whether a word is the shell's own
     * before it builds a pipeline can ask (specs/pipe.md). */
    static Builtin const *builtin_table(uint32_t &count) noexcept
    {
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
            {"newshell", &Shell::command_newshell},
            {"newcli", &Shell::command_newshell},
        };
        count = sizeof(kBuiltins) / sizeof(kBuiltins[0]);
        return kBuiltins;
    }

    static bool is_builtin_name(std::string const &name)
    {
        uint32_t count = 0;
        Builtin const *const table = builtin_table(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (equals_ci(name, table[i].name)) {
                return true;
            }
        }
        return false;
    }

    /* The boot script has finished: report the outcome to auth once, and only
     * once, and on failure put the read-only view up (specs/boot.md). */
    void boot_done()
    {
        if (!booting_) {
            return;
        }
        booting_ = false;
        uint64_t const status = (boot_failed_ || boot_forced_fail_) ? 10 : 0;
        if (status != 0) {
            /* The grid already holds what the sequence wrote; the terminal
             * shows it and takes no more input. */
            (void)aegir::console::stream_boot_fail(port_);
            boot_view_ = true;
        }
        if (boot_status_ != 0) {
            seL4_SetMR(0, status);
            seL4_Send(boot_status_, seL4_MessageInfo_new(0, 0, 0, 1));
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

    void command_cd(std::vector<std::string> const &args)
    {
        std::string const arg = join_words(args);
        if (arg.empty()) {
            print(current_directory() + "\n");
        } else if (!change_directory(arg)) {
            print("CD: not a directory: " + arg + "\n");
        }
    }

    void command_echo(std::vector<std::string> const &args)
    {
        print(join_words(args) + "\n");
    }

    /* EndCLI and EndShell are the same command -- EndCLI the older spelling,
     * EndShell the newer; both end this shell process (AmigaOS 3.1 reference).
     * Quit is a different thing: it aborts a script with a return code, and
     * belongs to the interpreter arc. */
    void command_endcli(std::vector<std::string> const &args)
    {
        static_cast<void>(args);
        print("bye\n");
        (void)aegir::console::stream_close(port_, 0);
        /* A boot script that ends with EndCLI closes before the frame could
         * empty, so the notification goes now (specs/boot.md). */
        boot_done();
        std::exit(0);
    }

    /* Newshell/Newcli start another terminal in a new window, carrying this
     * shell's context -- current directory, prompt, path, environment and
     * stack (specs/launch.md). The launcher owns the spawn authority, so this
     * is a launch of kind 3, the same call a MultiView or a desktop icon will
     * make. The arguments are the Amiga's: an optional window specification
     * (`WINDOW=<spec>`, or a bare `CON:...`) and `FROM <file>` for the new
     * shell's startup instead of S:Shell-Startup. The window rides the
     * request's own field; the FROM file rides as the program's arguments, so
     * the new terminal hands it to the shell it starts. */
    void command_newshell(std::vector<std::string> const &args)
    {
        std::string window;
        std::string from;
        for (std::size_t i = 0; i < args.size(); ++i) {
            std::string const &arg = args[i];
            if (arg.rfind("WINDOW=", 0) == 0) {
                window = arg.substr(7);
            } else if (arg.rfind("CON:", 0) == 0) {
                window = arg;
            } else if (arg == "FROM" && i + 1 < args.size()) {
                from = args[++i];
            }
        }
        std::string argv = "aegir-terminal";
        if (!from.empty()) {
            argv.push_back('\0');
            argv.append("FROM");
            argv.push_back('\0');
            argv.append(from);
        }
        if (!aegir::launch::spawn(argv.data(), static_cast<uint32_t>(argv.size()),
                                  aegir::launch::kKindLaunching, window.data(),
                                  static_cast<uint32_t>(window.size()))) {
            print("Newshell: the launcher would not start it\n");
            line_status_ = 10;
        }
    }

    void command_set(std::vector<std::string> const &args)
    {
        if (args.empty()) {
            print("Set: a name and a value, please\n");
            return;
        }
        std::string const &name = args[0];
        std::string const value = join_words(args, 1);
        if (!aegir::environment::setenv(name.c_str(), value.c_str())) {
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

    void command_get(std::vector<std::string> const &args)
    {
        if (args.empty()) {
            print("Get: what variable?\n");
            return;
        }
        std::string const &name = args[0];
        char const *const value = aegir::environment::getenv(name.c_str());
        print(name + "=" + (value != nullptr ? value : "(not set)") + "\n");
    }

    void command_unset(std::vector<std::string> const &args)
    {
        if (args.empty()) {
            print("UnSet: what variable?\n");
            return;
        }
        std::string const &name = args[0];
        aegir::environment::unsetenv(name.c_str());
        /* The persisted copy is the union's create target; removing it lets an
         * inherited base value show through again (specs/environment.md). */
        std::remove(("ENV:" + name).c_str());
    }

    void set_alias(std::string const &name, std::string value)
    {
        for (auto &alias : aliases_) {
            if (alias.first == name) {
                alias.second = std::move(value);
                return;
            }
        }
        aliases_.emplace_back(name, std::move(value));
    }

    /* Alias's argument is raw text: the value keeps its original spelling and
     * quoting, and is not substituted until the alias is used. `Alias` alone
     * lists, `Alias name` reports one, `Alias name value` defines it. */
    void command_alias(std::string const &rest)
    {
        std::string name;
        std::string value;
        split_word(rest, name, value);
        if (name.empty()) {
            for (auto const &alias : aliases_) {
                print(alias.first + "=" + alias.second + "\n");
            }
            return;
        }
        if (value.empty()) {
            for (auto const &alias : aliases_) {
                if (alias.first == name) {
                    print(name + "=" + alias.second + "\n");
                    return;
                }
            }
            print("Alias: " + name + " is not defined\n");
            return;
        }
        set_alias(name, value);
    }

    void command_unalias(std::vector<std::string> const &args)
    {
        if (args.empty()) {
            print("UnAlias: what alias?\n");
            return;
        }
        std::string const name = args[0];
        for (auto it = aliases_.begin(); it != aliases_.end(); ++it) {
            if (it->first == name) {
                aliases_.erase(it);
                return;
            }
        }
        print("UnAlias: " + name + " is not defined\n");
    }

    void command_prompt(std::vector<std::string> const &args)
    {
        /* No argument restores the directory-shaped default (specs/shell.md). */
        prompt_override_ = join_words(args);
        refresh_prompt();
    }

    void command_why(std::vector<std::string> const &args)
    {
        uint64_t code = last_status_;
        if (!args.empty()) {
            uint64_t parsed = 0;
            if (!parse_number(args[0], parsed)) {
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
    void command_eval(std::vector<std::string> const &args)
    {
        if (args.empty()) {
            print("Eval: what line?\n");
            line_status_ = 5;
            return;
        }
        (void)frames_.push(std::string{},
                           aegir::script::script_lines(join_words(args)));
    }

    /* Execute runs a command file (specs/shell.md). The file is read through
     * the session's namespace, its executable lines become a frame, and the
     * loop takes them from there; a file already running is a cycle and is
     * refused rather than recursed. The rest of the line is the file's
     * arguments: `$1..$n`, and the names a `.KEY` declares. */
    void command_execute(std::vector<std::string> const &args)
    {
        if (args.empty()) {
            print("Execute: which command file?\n");
            line_status_ = 10;
            return;
        }
        std::string const path = resolve(args[0]);
        std::vector<std::string> const supplied(args.begin() + 1, args.end());
        switch (start_script(path, supplied)) {
        case ScriptStart::Started:
            return;
        case ScriptStart::Cycle:
            print("Execute: " + args[0] + ": already running\n");
            break;
        case ScriptStart::NotFound:
            print("Execute: " + args[0] + ": not found\n");
            break;
        case ScriptStart::Invalid:
            /* start_script already said which declaration did not match. */
            break;
        }
        line_status_ = 10;
    }

    /* Run a command in the background (specs/shell.md's `Run`): the words
     * after the word, with the line's redirections, handed to the terminal to
     * start without waiting. The shell draws the next prompt while the command
     * runs; its output shares the console unless a redirection names a file or
     * a pipe (`Run >PIPE:name producer`). The terminal owns the spawn
     * authority, so this is the same call the foreground path makes, with the
     * background method (specs/terminal.md). */
    void run_background(std::vector<std::string> const &args, Redirect const &redirect)
    {
        if (args.empty()) {
            print("Run: which command?\n");
            line_status_ = 10;
            return;
        }
        /* The command word is taken as typed (specs/dos.md) and the words
         * travel NUL-separated, so an argument a quote grouped reaches the
         * command as one argument. */
        std::string payload = args[0];
        for (std::size_t i = 1; i < args.size(); ++i) {
            payload.push_back('\0');
            payload += args[i];
        }
        if (!aegir::launch::command(
                payload.data(), static_cast<uint32_t>(payload.size()),
                redirect.in_path.data(),
                static_cast<uint32_t>(redirect.in_path.size()),
                redirect.out_path.data(),
                static_cast<uint32_t>(redirect.out_path.size()), true)) {
            print("Run: " + args[0] + ": not started\n");
            line_status_ = 10;
        }
    }

    /* Quit ends the current command file with the return code it is given
     * (specs/shell.md's interpreter). From the prompt it says so rather than
     * ending the session -- EndCLI is the session's end. */
    void command_quit(std::vector<std::string> const &args)
    {
        uint64_t code = 0;
        if (!args.empty() && !parse_number(args[0], code)) {
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
    void command_failat(std::vector<std::string> const &args)
    {
        if (args.empty()) {
            print("FailAt " + std::to_string(frames_.fail_level()) + "\n");
            return;
        }
        uint64_t level = 0;
        if (!parse_number(args[0], level)) {
            print("FailAt: a return code level, please\n");
            line_status_ = 5;
            return;
        }
        frames_.set_fail_level(level);
    }

    /* Evaluate an If condition. EXISTS is the session's filesystem, and the
     * return-code words are the Amiga's thresholds; a comparison is
     * aegir::script's (specs/shell.md). */
    bool condition_holds(aegir::script::Condition const &condition)
    {
        bool value = false;
        switch (condition.kind) {
        case aegir::script::Condition::Kind::Exists: {
            std::error_code error;
            value =
                std::filesystem::exists(resolve(condition.path), error) && !error;
            break;
        }
        case aegir::script::Condition::Kind::Warn:
            value = last_status_ >= 5;
            break;
        case aegir::script::Condition::Kind::Error:
            value = last_status_ >= 10;
            break;
        case aegir::script::Condition::Kind::Fail:
            value = last_status_ >= 20;
            break;
        case aegir::script::Condition::Kind::Compare:
            value = aegir::script::compare_strings(condition.op, condition.lhs,
                                                   condition.rhs);
            break;
        }
        return condition.negate ? !value : value;
    }

    /* Lab marks a line a Skip can name; reached on its own it does nothing. The
     * name is literal -- a Skip finds the label in the file's text, which the
     * control-word intercept guarantees is not a substituted word. */
    void command_lab(std::string const &rest)
    {
        if (rest.empty()) {
            print("Lab: a label name, please\n");
            line_status_ = 10;
        }
    }

    /* Skip moves the running command file to the line after its label. */
    void command_skip(std::string const &rest)
    {
        std::string name;
        std::string extra;
        split_word(rest, name, extra);
        if (name.empty()) {
            print("Skip: a label name, please\n");
            line_status_ = 10;
            return;
        }
        if (!frames_.jump_to_label(name)) {
            print("Skip: no label " + name + "\n");
            line_status_ = 10;
        }
    }

    /* If runs the then-body when the condition holds and steps to the matching
     * Else or EndIf when it does not. The condition is substituted first, so a
     * variable can be an operand; a condition that will not parse is an error
     * and steps past the whole block. */
    void command_if(std::string const &rest)
    {
        std::vector<std::string> words;
        std::string error;
        aegir::script::Arguments const empty;
        aegir::script::Arguments const *const arguments = frames_.current_arguments();
        bool parsed = aegir::script::substitute_words(
            rest, arguments != nullptr ? *arguments : empty, environment_lookup,
            words, error);
        aegir::script::Condition condition;
        if (parsed) {
            parsed = aegir::script::parse_condition(words, condition, error);
        }
        if (!parsed) {
            print("If: " + error + "\n");
            line_status_ = 10;
            (void)frames_.skip_to_endif();
            return;
        }
        if (!condition_holds(condition) && !frames_.skip_to_else_or_endif()) {
            print("If: no EndIf\n");
            line_status_ = 20;
        }
    }

    /* Else reached while running means the If was true, so its else body must
     * not run: step past the matching EndIf. */
    void command_else()
    {
        if (!frames_.skip_to_endif()) {
            print("Else: no EndIf\n");
            line_status_ = 20;
        }
    }

    /* Run a pipeline: every stage a program, connected by pipes the terminal
     * names (specs/pipe.md). A stage that is a built-in is refused -- the
     * shell's own words are not programs the terminal can start -- and an
     * empty stage is a syntax error. True when the terminal started it. */
    bool run_pipeline(std::vector<std::vector<std::string>> const &stages)
    {
        std::vector<std::string> lines;
        std::vector<std::string> ins;
        std::vector<std::string> outs;
        for (std::vector<std::string> const &stage : stages) {
            Redirect const redirect = split_redirect(stage);
            if (redirect.words.empty()) {
                print("Pipe: an empty stage\n");
                line_status_ = 10;
                return false;
            }
            std::string const command = redirect.words[0];
            if (is_builtin_name(command)) {
                print("Pipe: " + redirect.words[0] + " is a built-in\n");
                line_status_ = 10;
                return false;
            }
            std::string payload = command;
            for (std::size_t i = 1; i < redirect.words.size(); ++i) {
                payload.push_back('\0');
                payload += redirect.words[i];
            }
            lines.push_back(std::move(payload));
            ins.push_back(redirect.in_path);
            outs.push_back(redirect.out_path);
        }
        std::vector<aegir::launch::Stage> wire;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            wire.push_back({lines[i].data(), static_cast<uint32_t>(lines[i].size()),
                            ins[i].data(), static_cast<uint32_t>(ins[i].size()),
                            outs[i].data(), static_cast<uint32_t>(outs[i].size())});
        }
        /* The stages' pids come back in the answer (specs/launch.md): the shell
         * announces them on its stream so the terminal's Ctrl-C can set **C**
         * on every stage, not one (specs/process.md). */
        std::vector<uint64_t> pids(stages.size(), 0);
        uint32_t pid_count = 0;
        if (aegir::launch::pipeline(wire.data(), static_cast<uint32_t>(wire.size()),
                                    pids.data(), static_cast<uint32_t>(pids.size()),
                                    &pid_count)) {
            if (pid_count == pids.size()) {
                aegir::console::stream_line(port_, static_cast<uint32_t>(pids.size()),
                                            pids.data());
            }
            busy_ = true;
            return true;
        }
        print("Pipe: the terminal would not start it\n");
        line_status_ = 10;
        return false;
    }

    /* Run one line: true when it started a program (busy_ is set and the exit
     * comes later), false when it finished here (a built-in, a directory
     * change, an unknown name). line_status_ is the line's own return code,
     * which the fail level checks for a built-in. */
    bool run_line(std::string const &line)
    {
        line_status_ = 0;
        /* Control flow is structural: its word is read from the line as it was
         * written, before aliases or substitution, so the shape a Skip walks
         * and an If matches is the shape the file has (specs/shell.md). An If's
         * condition is substituted inside the handler; a label is literal. */
        std::string control_first;
        std::string control_rest;
        split_word(line, control_first, control_rest);
        std::string const control = aegir::script::control_word(line);
        if (!control.empty()) {
            if (control == "lab") {
                command_lab(control_rest);
            } else if (control == "skip") {
                command_skip(control_rest);
            } else if (control == "if") {
                command_if(control_rest);
            } else if (control == "else") {
                command_else();
            }
            /* EndIf is where a block ends and does nothing itself. */
            return false;
        }
        /* An alias is folded in first, as raw text, so a variable in its value
         * expands at use, not at definition (expand_aliases, specs/shell.md).
         * `Alias` then takes the rest of the line raw, because that value must
         * not be substituted until the alias is used; every other line is
         * substituted here, before a command, a directory or an alias is
         * looked for. The arguments of a running command file are what
         * `$1..$n` and `{name}` read; a console line has none. */
        std::string const expanded = aegir::script::expand_aliases(line, aliases_);
        std::string first;
        std::string rest;
        split_word(expanded, first, rest);
        if (equals_ci(first, "alias")) {
            command_alias(rest);
            return false;
        }
        aegir::script::Arguments const empty;
        aegir::script::Arguments const *const frame_arguments =
            frames_.current_arguments();
        std::vector<std::string> words;
        std::string error;
        if (!aegir::script::substitute_words(
                expanded, frame_arguments != nullptr ? *frame_arguments : empty,
                environment_lookup, words, error)) {
            print("Syntax error: " + error + "\n");
            line_status_ = 10;
            return false;
        }
        /* A line with a `|` is a pipeline: its stages are programs the
         * terminal starts at once, connected by pipes it names (specs/pipe.md).
         * A one-stage line is the ordinary path below. */
        std::vector<std::vector<std::string>> const stages = split_pipeline(words);
        /* A pipeline is a line with more than one stage: announce its stage
         * count before anything is launched (specs/signal.md), so the terminal
         * knows what the completion cue is read from. */
        if (stages.size() > 1) {
            aegir::console::stream_line(port_, static_cast<uint32_t>(stages.size()));
            return run_pipeline(stages);
        }
        Redirect const redirect = split_redirect(stages[0]);
        std::vector<std::string> const &command_words = redirect.words;
        if (command_words.empty()) {
            return false;
        }
        /* The command word is taken as typed: the shell rewrites nothing. A
         * volume name is case-insensitive (the VFS folds it, specs/vfs.md); a
         * filename's case is the underlying filesystem's business -- FAT folds,
         * BFS does not (specs/fat.md, specs/bfs.md). */
        std::string const command = command_words[0];
        std::vector<std::string> const args(command_words.begin() + 1,
                                            command_words.end());

        /* `Run` starts a program in the background (specs/shell.md): the
         * words after it are the command, and the line's redirection belongs
         * to that command, not to `Run` itself, so this is handled before the
         * built-in redirect path. */
        if (equals_ci(command, "run")) {
            run_background(args, redirect);
            return false;
        }

        uint32_t builtin_count = 0;
        Builtin const *const kBuiltins = builtin_table(builtin_count);
        for (uint32_t b = 0; b < builtin_count; ++b) {
            Builtin const &builtin = kBuiltins[b];
            if (equals_ci(command, builtin.name)) {
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
                    (this->*builtin.handler)(args);
                    redirect_out_ = nullptr;
                    std::fclose(file);
                } else {
                    (this->*builtin.handler)(args);
                }
                return false;
            }
        }

        /* Not a built-in: a directory typed on its own is the Amiga's implicit
         * change into it (specs/shell.md); anything else is a program. */
        if (is_directory(resolve(command_words[0]))) {
            (void)change_directory(command_words[0]);
            return false;
        }
        /* A program, launched on this shell's behalf (specs/launch.md): the
         * shell asks the session's launcher, which owns the spawn authority
         * and starts the command with this stream. The command word travels as
         * typed (specs/dos.md) and the words travel NUL-separated, so an
         * argument a quote grouped reaches the command as one argument
         * (specs/shell.md); the redirections already left the list. The launch
         * carries the shell's own context, so `cd` and `Set` reach the
         * command. */
        std::string payload = command;
        for (std::string const &arg : args) {
            payload.push_back('\0');
            payload += arg;
        }
        /* A foreground command: announce the one-stage line (specs/signal.md),
         * so the terminal knows a command is about to run on the stream -- it
         * routes keys to the command's input queue while the bracket holds --
         * and when the completion cue is due. A `Run` does not announce: it
         * keeps the shell's line editor while it runs. */
        aegir::console::stream_line(port_, 1);
        uint64_t badge = 0;
        if (aegir::launch::command(
                payload.data(), static_cast<uint32_t>(payload.size()),
                redirect.in_path.data(),
                static_cast<uint32_t>(redirect.in_path.size()),
                redirect.out_path.data(),
                static_cast<uint32_t>(redirect.out_path.size()), false, &badge)) {
            /* The command's pid, from the launch answer (specs/launch.md): the
             * terminal's Ctrl-C sets **C** on it (specs/process.md). */
            if (badge != 0) {
                aegir::console::stream_line(port_, 1, &badge);
            }
            busy_ = true;
            return true;
        }
        print("Unknown command: " + command_words[0] + "\n");
        line_status_ = 10;
        return false;
    }

    aegir::ipc::Consumer port_;
    bool busy_ = false;
    aegir::script::Frames frames_;
    std::vector<std::pair<std::string, std::string>> aliases_;
    std::string prompt_override_;
    uint64_t last_status_ = 0;
    uint64_t line_status_ = 0;
    /* The sink a built-in's output uses while a redirected line runs, or null
     * for the console stream (specs/shell.md). */
    std::FILE *redirect_out_ = nullptr;
    /* The boot session's status endpoint, sent when Startup-Sequence is done
     * (specs/boot.md); zero for a shell that is not the boot shell. */
    seL4_CPtr boot_status_ = 0;
    bool booting_ = false;
    /* A command the boot script ran failed at or above the fail level, or the
     * firmware's aegir.fail forced it: the outcome is a failure. */
    bool boot_failed_ = false;
    bool boot_forced_fail_ = false;
    /* The failure view is up: the loop stops reading (specs/boot.md). */
    bool boot_view_ = false;
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

    Shell shell(port);
    shell.set_boot_status();
    shell.load_environment();
    shell.start();
    /* A shell started with a command file is the boot session when it also
     * holds the boot status channel: it runs Startup-Sequence and signals auth
     * when it is done. A shell started with a file but no such channel is a
     * `NEWSHELL FROM` startup (specs/launch.md): it runs the named file as its
     * startup. One started with none is interactive and runs Shell-Startup
     * (specs/shell.md). */
    /* `--session` (specs/session.md): the terminal auth started for the
     * session's own composition, so the shell runs `Home:S/User-Startup` once
     * before Shell-Startup. An argument, not an environment entry, because a
     * child inherits the environment and a nested shell must not run it. */
    if (argc > 1 && argv[1] != nullptr && std::string(argv[1]) == "--session") {
        shell.run_startup(true);
    } else if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
        if (shell.has_boot_status()) {
            shell.run_boot_script(argv[1]);
        } else {
            shell.run_startup_file(argv[1]);
        }
    } else {
        shell.run_startup(false);
    }
    shell.loop();
    return 0;
}