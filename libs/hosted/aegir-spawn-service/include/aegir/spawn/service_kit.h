/*
 * The spawn service's kit (specs/authority.md, specs/shell.md, specs/memory.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Auth delegates the authority to start a session's programs; one process uses
 * it per session -- the launcher, or for the boot session the terminal itself.
 * This is that process's side of the spawn machinery, in one place so the
 * headless launcher and the terminal share it: the staged allocator over the
 * command chunks, the reserved CSpace pool one owner per live command, and the
 * reap that returns both whole (specs/memory.md Phase 5).
 *
 * It is deliberately toolkit-free. A hosted process has one VSpace root, and
 * the process that spawns stages through the window it was given -- for the
 * toolkit that is its own, for the headless launcher `g_scratch` -- so the
 * caller hands in the allocator and scratch rather than the kit reaching for a
 * singleton. What it does *not* build is the first-class grants of a child
 * (aegir/spawn/kit.h does that); this owns the memory and slots a spawn runs
 * on.
 */

#ifndef AEGIR_SPAWN_SERVICE_KIT_H
#define AEGIR_SPAWN_SERVICE_KIT_H

#include <aegir/ipc/port.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/arena.h>
#include <aegir/mem/slot_pool.h>
#include <aegir/spawn/initrd.h>
#include <aegir/spawn/kit.h>
#include <aegir/spawn/process.h>
#include <sel4/sel4.h>

#include <memory>
#include <string>
#include <vector>

namespace aegir::spawn {

class ServiceKit {
public:
    /* The untyped a command's own runtime is given at spawn: its heap and page
     * tables are retyped from it, and it is the command's *first* chunk, not
     * its ceiling -- the runtime asks mem.main for more as it grows
     * (specs/memory.md). */
    static constexpr uint32_t kCommandUntypedBits = 20; /* 1 MiB */

    /* Adopt the delegated kit, read by name from the bootstrap block.
     * `allocator` and `scratch` are the process's own -- the toolkit's, or a
     * headless service's `g_objects`/`g_scratch` -- and `slot_base`/
     * `slot_count` is the CSpace range reserved for commands (the toolkit
     * reserves everything above its own slots; a headless service reserves the
     * whole CSpace and hands the pool out from the top, `slot_descend`, so its
     * own cursor below cannot meet it while anything is left). False when a
     * grant is missing or an endpoint cannot be made; the caller then runs
     * without a spawner. */
    bool adopt(aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch,
               uint64_t slot_base, uint32_t slot_count, bool slot_descend = false);

    bool ready() const { return ready_; }

    /* Stage one command's spawn, in the pool slots owned by `owner` (an id the
     * caller picks, one per live command): reset the allocator, point it at
     * that owner, and build the spawner. begin_command() then mints the memory
     * copy the command's chunks are owned by, and end_staging() drops the
     * staging and rewinds the window -- the command is alive, its capabilities
     * are in the pool, and the next command may be staged at once. */
    bool begin(uint32_t owner);
    bool begin_command(uint64_t badge);
    void end_staging();

    /* A command's staging that never produced a live command (a missing
     * image, a spawn that failed): release whatever its badge owns and return
     * its slots, then drop the staging. */
    void abandon(uint64_t badge, uint32_t owner);

    /* Rewind the staging window to where it stood before the first live
     * command was staged. Valid only when no command is live: the revoke that
     * reaped them unmapped their staging frames, and this is bookkeeping
     * (specs/memory.md Phase 5). */
    void rewind_staging();

    /* Stop and reclaim one command: suspend its TCB, release its memory by
     * badge -- the chunks' capabilities, the TCB among them, go with it -- and
     * return its pool slots. `tcb` may be zero when the command never ran. */
    void reap(seL4_CPtr tcb, uint64_t badge, uint32_t owner);

    Spawner& spawner() { return *spawner_; }
    /* The allocator over the command chunks: the spawner's objects and a
     * command's seed are retyped from it, and its untyped source is mem.main
     * (specs/memory.md). */
    aegir::mem::Allocator& memory();

    /* The first-class kit (specs/launch.md): the capabilities every child's
     * grant is built from. The caller builds a command's, a nested terminal's
     * and the shell's ports with aegir::spawn::command_ports / launcher_ports /
     * shell_ports over this, so the lists live in one place. It is mutable
     * because a launcher sets `stream` per request to the caller's own.
     */
    Kit& kit() { return kit_; }
    Kit const& kit() const { return kit_; }

    /* The current command's memory copy: minted from mem_port_ and badged with
     * the command's id, so the service records its chunks as that command's.
     * A copy of it goes to the command, so its own runtime grows within the
     * same ownership. Dropped by end_staging. */
    seL4_CPtr command_mem() const { return command_mem_; }

    /* Where a command's faults arrive. Tier 1 does not read it. */
    seL4_CPtr fault_endpoint() const { return fault_endpoint_; }

    /* The shell process: spawned once from auth's `shell-pool`, not pooled and
     * reclaimed like a command, because it lives as long as its terminal. It
     * runs on `badge` -- the stream key its con.stream copy carries -- and the
     * terminal serves it like any other client. `arguments` are what follow
     * argv[0] (specs/environment.md): for the boot session, the command file
     * the shell is to run. */
    bool spawn_shell(char const *image, uint64_t image_bytes, char const *cwd,
                     uint32_t cwd_length, uint64_t badge, char const *const *arguments,
                     uint32_t argument_count);

    /* The launcher kit a child is built from: true when an unbadged console.gui
     * was delegated, so a nested terminal can mint its own. */
    bool can_launch() const { return kit_.console_gui != 0; }

    /* The boot session's status endpoint, when this process is the boot
     * session's (auth grants it as `boot.status`): the shell sends the outcome
     * -- 0 success, nonzero failure -- and auth receives it (specs/boot.md).
     * Zero for an interactive session. */
    seL4_CPtr boot_status() const { return boot_status_; }

    /* ---- starting programs: the spawn paths (specs/launch.md) ---- */

    /* This process's identity for the badges it hands out: its own badge, and
     * the range auth delegated for its children (specs/launch.md). A system
     * badge has no range, and its children keep the small system serials. */
    void set_identity(uint64_t own_badge, uint64_t range_base, uint64_t range_size);
    /* The same, reading the range out of this process's AEGIR_BADGE_RANGE: a
     * malformed value is refused rather than guessed, so a launcher is never
     * handed someone else's serials. */
    void adopt_identity(uint64_t own_badge);
    bool have_range() const { return badge_size_ != 0; }

    /* Load a program's bytes through the session namespace, from a path that is
     * an assign or a volume (specs/dos.md): `path` is already resolved by the
     * caller. The buffer is kept and reused, so a spawn's image does not map
     * fresh pages each command. The bytes are the last load's, for spawn_shell
     * and for one command's spawn. */
    bool load_image(std::string const &path);
    char const *image() const { return image_.data(); }
    uint64_t image_bytes() const { return image_.size(); }

    /* One command to start: its words (program first, the rest as written), the
     * caller's context, and where its output goes. `stream` is the con.stream
     * the command writes to -- a launcher's own for a boot command, or the
     * capability the caller sent for a session command -- and `stream_copy` says
     * it is already badged, so it is copied rather than minted. */
    struct Command {
        std::vector<std::string> const *words = nullptr;
        std::string const *cwd = nullptr;
        std::string const *environment = nullptr; /* NUL-separated NAME=VALUE */
        /* The caller's Path search list (`C:` today, `specs/dos.md`): a bare
         * program name is searched across it in order. Empty means the default,
         * one `C:` entry. */
        std::string const *path = nullptr;
        std::string const *std_in = nullptr;
        std::string const *std_out = nullptr;
        bool background = false;
        uint32_t stack_pages = 0;
        seL4_CPtr stream = 0;
        uint64_t stream_badge = 0;
        bool stream_copy = false;
        /* Start an output view (specs/launch.md): the child serves the stream
         * rather than writing to it, so its grant is `output_ports` -- the
         * endpoint as an owner copy, plus the launcher's `launch.session` half
         * so it can release the commands whose exits it reports. */
        bool output_view = false;
        /* Start a class (specs/datatypes.md): the request's `stream` is a serve
         * port the caller made, installed as `datatypes.class`, and the child
         * serves it instead of writing a stream. It runs under the caller's
         * badge, like a command. */
        bool serve = false;
    };

    /* One pipeline stage on the wire: its command line and its own
     * redirections, empty for the console or the connecting pipe. */
    struct Stage {
        std::string line;
        std::string std_in;
        std::string std_out;
    };

    /* What a spawn produced: the process, and the identity its memory and pool
     * slots are owned by (specs/memory.md Phase 5). */
    struct Started {
        Process process;
        uint64_t badge = 0;
        uint32_t owner = 0;
        /* A `Run` (specs/shell.md): the command does not hold the caller's
         * line, so its exit is reported apart from the line's (specs/terminal.md). */
        bool background = false;
    };

    /* Start one command: mint its badge and memory owner, stage it, spawn it,
     * and remember it so release() can take it back. False when the image is
     * missing or the spawn fails. */
    bool start_command(Command const &command, Started *out);

    /* Start the launcher's output view (specs/launch.md), once: a program that
     * serves a read-only con.stream for a command a caller launched with no
     * stream of its own, and releases each command whose exit it reports. The
     * endpoint the view owns is `output_stream()`; the launcher hands its
     * commands a badged copy of it. True when a view is already up or one
     * started, false when it could not -- the caller then refuses the request. */
    bool start_output_view(Started *out);

    /* The view's con.stream endpoint, or zero when none is up. */
    seL4_CPtr output_stream() const { return output_view_endpoint_; }

    /* Start a pipeline: every stage at once, connected by pipes this service
     * names (specs/pipe.md), so no stage depends on a name the user chose. The
     * context -- cwd, environment, background, stack and the stream every stage
     * shares -- comes from `context`; the stages carry only their lines and
     * redirections. `out` receives the stages' records (up to `out_capacity`).
     * False when a stage would not start; the stages already started are
     * released. */
    bool start_pipeline(Stage const *stages, uint32_t count, Command const &context,
                        Started *out, uint32_t out_capacity);

    /* Start a launching program (a nested terminal): a peer with its own
     * console window and its own badge range, its memory drawn from mem.main
     * under this process's badge (specs/launch.md). `arguments` are what follow
     * argv[0] (a `FROM <file>`); `default_window` is the window specification
     * when the caller gave none. */
    bool start_launcher(std::string const &program, std::string const &window,
                        std::vector<std::string> const &arguments,
                        std::string const &default_window, std::string const &cwd,
                        Started *out);

    /* Take a command back by badge: suspend it, release its memory, return its
     * slots (specs/memory.md Phase 5). Answers 0 for a badge no live command
     * carries, 1 for a command that held the caller's line, 2 for a background
     * `Run` -- so the stream that saw the exit can report it apart. */
    int release(uint64_t badge);
    bool live() const { return !live_.empty(); }

    /* A launcher's commands write to a stream it was handed, not to one it
     * made: move the capability the last call carried into the service's own
     * slot and hand it to the next command (specs/launch.md). False when the
     * move fails; the caller then refuses the request. */
    bool hold_received_stream();
    seL4_CPtr received_stream() const { return stream_slot_; }

    /* Split a NUL-separated argv string into words -- a caller parsing the
     * wire's one argv field, or this class splitting a pipeline stage's line. */
    static std::vector<std::string> split_words(std::string const &text);

private:
    /* The next badge for a child: a user serial out of the range, or a small
     * system serial when there is none. Zero when the range is spent. */
    uint64_t take_badge();
    uint32_t take_owner();
    /* Split a NUL-separated environment into pointers. */
    static void split_environment(std::string const &text,
                                  std::vector<std::string> &storage,
                                  std::vector<char const *> &pointers);
    Started *find_live(uint64_t badge);
    /* An untyped of `bits` from mem.main through the current command's badged
     * copy (specs/memory.md): a nested terminal's runtime and shell pool. */
    seL4_CPtr alloc_child_mem(uint32_t bits);
    /* The next nested-terminal quarter out of the range (specs/launch.md):
     * false when the range is spent or there is none. */
    bool reserve_child(uint64_t *base, uint64_t *badge);
    /* Load a program's image (specs/dos.md): a name with an assign/volume is
     * used as typed; a name with `/` is resolved against the caller's directory;
     * a bare name is lowercased and searched across the caller's Path (one `C:`
     * entry when the request gave none). */
    bool load_program(std::string const &name, std::string const *path,
                      std::string const *cwd);

    uint64_t own_badge_ = 0;
    uint64_t badge_base_ = 0;
    uint64_t badge_size_ = 0;
    uint64_t next_badge_ = 0;
    uint64_t child_count_ = 0;
    uint64_t command_serial_ = 0;
    uint64_t pipeline_serial_ = 0;
    uint64_t owner_serial_ = 0;
    std::vector<char> image_;
    std::vector<Started> live_;

    aegir::mem::Allocator *allocator_ = nullptr;
    aegir::mem::Scratch *scratch_ = nullptr;
    uint64_t slot_base_ = 0;
    uint32_t slot_count_ = 0;
    /* Whether the command pool comes off the top of its range (adopt). */
    bool slot_descend_ = false;
    aegir::mem::Account account_{"spawn-service", 0, 0, 0};
    std::unique_ptr<aegir::mem::Arena> arena_;
    std::unique_ptr<aegir::spawn::Initrd> initrd_;
    std::unique_ptr<aegir::spawn::Spawner> spawner_;
    /* The reserved command-slot pool: one owner per live command, so reaping
     * one returns only its slots (specs/memory.md Phase 5). */
    aegir::mem::SlotPool slot_pool_;
    std::vector<uint32_t> slot_owners_;
    seL4_CPtr stream_endpoint_ = 0;
    seL4_CPtr fault_endpoint_ = 0;
    /* The output view's endpoint, once one is up (specs/launch.md): the view
     * owns it, and a stream-less command is handed a badged copy. */
    seL4_CPtr output_view_endpoint_ = 0;
    /* The slot the last received stream capability was moved into, reused per
     * request (a launcher's commands write to the caller's stream). */
    seL4_CPtr stream_slot_ = 0;
    bool stream_live_ = false;
    seL4_CPtr log_port_ = 0;
    seL4_CPtr nmspace_port_ = 0;
    seL4_CPtr command_nmspace_port_ = 0;
    seL4_CPtr command_clock_port_ = 0;
    seL4_CPtr command_timer_port_ = 0;
    seL4_CPtr boot_status_ = 0;
    seL4_CPtr asid_pool_ = 0;
    /* The unbadged mem.main copy, and the per-command copy minted from it
     * (specs/memory.md). The command copy lives in one toolkit slot, re-minted
     * for each command while it is staged, and dropped once it is spawned --
     * the command holds its own copy. */
    seL4_CPtr mem_port_ = 0;
    seL4_CPtr command_mem_ = 0;
    bool command_mem_live_ = false;
    /* The first-class kit every child's grant is built from (specs/launch.md). */
    aegir::spawn::Kit kit_{};
    seL4_CPtr shell_pool_ = 0;
    uint32_t shell_pool_bits_ = 0;
    uintptr_t scratch_mark_ = 0;
    bool staged_since_rewind_ = false;
    bool ready_ = false;
};

}  // namespace aegir::spawn

#endif  // AEGIR_SPAWN_SERVICE_KIT_H
