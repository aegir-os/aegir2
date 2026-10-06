/*
 * The spawn service's spawn paths (specs/launch.md, specs/pipe.md, specs/dos.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * service_kit.h owns the memory and slots a spawn runs on; this owns what a
 * spawn *is*: resolving a program's image through the session namespace,
 * minting the badge a command's memory is owned by, building the grants from
 * the first-class kit, and starting the process. The launcher and the boot
 * terminal both come through here, so a command started by either is the same
 * shape.
 */

#include <aegir/spawn/service_kit.h>

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/environment.h>
#include <aegir/heap.h>
#include <aegir/ipc/port.h>
#include <aegir/memory.h>
#include <aegir/nmspace.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <string>
#include <vector>

namespace aegir::spawn {

namespace {

/* The pipe that connects two stages: named by the spawner, so a pipeline never
 * depends on a name the user chose (specs/pipe.md). */
std::string pipe_path(uint64_t serial, uint32_t index)
{
    return std::string("PIPE:p") + std::to_string(serial) + "_" + std::to_string(index);
}

/* The range a serial space one process owns: a quarter of the delegated range
 * is reserved for nested terminals (specs/launch.md). */
uint64_t child_quarter(uint64_t badge_size)
{
    return badge_size / 4;
}

}  // namespace

void ServiceKit::set_identity(uint64_t own_badge, uint64_t range_base, uint64_t range_size)
{
    own_badge_ = own_badge;
    badge_base_ = range_base;
    badge_size_ = range_size;
    next_badge_ = range_base;
}

void ServiceKit::adopt_identity(uint64_t own_badge)
{
    /* The range travels as AEGIR_BADGE_RANGE=<base>,<size> (specs/launch.md).
     * A malformed value is refused rather than guessed: a launcher handed
     * someone else's serials would collide with another session's. */
    char const *const value = aegir::environment::getenv("AEGIR_BADGE_RANGE");
    if (value == nullptr) {
        set_identity(own_badge, 0, 0);
        return;
    }
    uint64_t values[2] = {0, 0};
    uint32_t which = 0;
    bool any_digit = false;
    for (char const *p = value;; ++p) {
        char const c = *p;
        if (c >= '0' && c <= '9') {
            values[which] = values[which] * 10 + static_cast<uint64_t>(c - '0');
            any_digit = true;
        } else if (c == ',' && which == 0) {
            if (!any_digit) {
                set_identity(own_badge, 0, 0);
                return;
            }
            which = 1;
            any_digit = false;
        } else if (c == '\0') {
            if (!any_digit || which != 1 || values[1] == 0) {
                set_identity(own_badge, 0, 0);
                return;
            }
            set_identity(own_badge, values[0], values[1]);
            return;
        } else {
            set_identity(own_badge, 0, 0);
            return;
        }
    }
}

namespace {

/* Read exactly `length` bytes at `offset` into `destination`; false on a short
 * read. The offset is the file's, not the fd cursor's, so the spawner can pull
 * segments in whatever order the ELF names them. */
bool read_at(int fd, uint64_t offset, uint64_t length, void *destination) noexcept
{
    if (::lseek(fd, static_cast<long>(offset), SEEK_SET) != static_cast<long>(offset)) {
        return false;
    }
    auto *out = static_cast<char *>(destination);
    uint64_t total = 0;
    while (total < length) {
        long const got = ::read(fd, out + total, length - total);
        if (got <= 0) {
            return false;
        }
        total += static_cast<uint64_t>(got);
    }
    return true;
}

uint16_t read_le16(uint8_t const *at) noexcept
{
    return static_cast<uint16_t>(static_cast<uint16_t>(at[0]) |
                                 static_cast<uint16_t>(static_cast<uint16_t>(at[1]) << 8));
}

uint64_t read_le64(uint8_t const *at) noexcept
{
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | at[i];
    }
    return value;
}

/* The most of an image the ELF's own headers may need held: the header and the
 * program header table. A program whose table is larger is refused as malformed
 * rather than read whole. */
constexpr uint64_t kImagePrefixMax = 64 * 1024;

}  // namespace

bool ServiceKit::load_image(std::string const &path)
{
    if (image_fd_ >= 0) {
        ::close(image_fd_);
        image_fd_ = -1;
    }
    int const fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    /* The size comes from the fd already open, not a second resolve by path:
     * the fd holds the volume capability, and a path stat would ask the
     * namespace to resolve the same file again. */
    struct stat info {};
    uint64_t size = 0;
    if (::fstat(fd, &info) == 0 && info.st_size > 0) {
        size = static_cast<uint64_t>(info.st_size);
    }
    /* Read only the ELF header and the program header table -- enough for the
     * spawner to parse the image -- and keep the fd open so the spawner reads
     * each segment straight into the child's frames (Request::image_source).
     * The whole image is never held here (specs/director.md's spawn path). */
    uint8_t header[64];
    if (size < sizeof(header) || !read_at(fd, 0, sizeof(header), header)) {
        ::close(fd);
        return false;
    }
    /* ELF64: e_phoff at 32, e_phentsize at 54, e_phnum at 56. */
    uint64_t const phoff = read_le64(header + 32);
    uint16_t const phentsize = read_le16(header + 54);
    uint16_t const phnum = read_le16(header + 56);
    uint64_t const prefix =
        phoff + static_cast<uint64_t>(phentsize) * static_cast<uint64_t>(phnum);
    if (phentsize == 0 || phnum == 0 || prefix > size || prefix > kImagePrefixMax) {
        ::close(fd);
        return false;
    }
    image_.resize(static_cast<std::size_t>(prefix));
    if (!read_at(fd, 0, prefix, image_.data())) {
        image_.clear();
        ::close(fd);
        return false;
    }
    image_fd_ = fd;
    image_size_ = size;
    image_path_ = path;
    return true;
}

bool ServiceKit::fetch_image(void *context, uint64_t offset, uint64_t length,
                             void *destination) noexcept
{
    auto *const kit = static_cast<ServiceKit *>(context);
    if (kit == nullptr || kit->image_fd_ < 0) {
        return false;
    }
    return read_at(kit->image_fd_, offset, length, destination);
}

bool ServiceKit::fetch_image_frame(void *context, uint64_t offset, uint64_t length,
                                   uint64_t frame_offset, seL4_CPtr frame,
                                   uint32_t frame_bits) noexcept
{
    auto *const kit = static_cast<ServiceKit *>(context);
    if (kit == nullptr || kit->image_fd_ < 0) {
        return false;
    }
    /* The bulk path: the filesystem maps our frame and writes the bytes into it,
     * one call for the page (aegir/volume.h's read-frame). A filesystem that
     * does not serve read-frame refuses it, and the frame is filled through our
     * own window instead -- the inline read this stands in for, so a volume
     * that has not learned read-frame still loads. */
    if (aegir::heap::files::read_frame(kit->image_fd_, offset, frame_offset, length, frame,
                                       frame_bits) == static_cast<long>(length)) {
        return true;
    }
    if (kit->scratch_ == nullptr || frame_bits > seL4_PageBits) {
        /* The window path fills a 4 KiB page; a mega page has no window here,
         * so a filesystem that cannot serve it is a hard failure. */
        return false;
    }
    void *window = kit->scratch_->map(frame);
    if (window == nullptr) {
        return false;
    }
    bool const ok =
        read_at(kit->image_fd_, offset, length, static_cast<char *>(window) + frame_offset);
    kit->scratch_->unmap(frame);
    return ok;
}

/* The directory a spawned program's own binary came from (specs/environment.md):
 * the resolved image path's directory, or empty for an initrd read -- a flat
 * image has no directory. A child finds its own libraries and classes beside
 * its binary, whatever the current directory is. */
std::string program_directory(std::string const &path)
{
    if (path.rfind("Initrd:", 0) == 0) {
        return {};
    }
    std::size_t const cut = path.find_last_of(":/");
    if (cut == std::string::npos) {
        return {};
    }
    return path.substr(0, cut + 1);
}

bool ServiceKit::load_program(std::string const &name, std::string const *path,
                              std::string const *cwd)
{
    /* A path is used as typed: `Sys:System/Terminal`, `BD1:Foo` (specs/dos.md).
     * An assign or a volume is the namespace's to resolve. */
    if (name.find(':') != std::string::npos) {
        return load_image(name);
    }
    /* A path with `/` is resolved against the caller's current directory, so a
     * relative `Sub/Prog` reaches `Home:rroland/Sub/Prog`. */
    if (name.find('/') != std::string::npos) {
        if (cwd == nullptr) {
            return load_image(name);
        }
        std::string candidate(*cwd);
        if (!candidate.empty() && candidate.back() != '/') {
            candidate.push_back('/');
        }
        candidate.append(name);
        return load_image(candidate);
    }
    /* A bare name: lowercase it -- the convention that reconciles the Amiga's
     * case-blindness with Aegir's case-sensitive filesystems -- and search the
     * caller's Path entries in order. One `C:` entry is what a system without a
     * Path ships (specs/dos.md). */
    std::string lowered;
    lowered.reserve(name.size());
    for (char const c : name) {
        lowered.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    std::string const entries =
        path != nullptr && !path->empty() ? *path : std::string("C:");
    std::size_t start = 0;
    for (std::size_t i = 0; i <= entries.size(); ++i) {
        if (i != entries.size() && entries[i] != ';') {
            continue;
        }
        std::string entry = entries.substr(start, i - start);
        start = i + 1;
        if (entry.empty()) {
            continue;
        }
        if (entry.back() != ':') {
            entry.push_back(':');
        }
        entry.append(lowered);
        if (load_image(entry)) {
            return true;
        }
    }
    return false;
}

uint64_t ServiceKit::take_badge()
{
    if (aegir::ipc::is_user_badge(own_badge_)) {
        uint64_t const child_size = child_quarter(badge_size_);
        uint64_t const floor =
            have_range() ? badge_base_ + badge_size_ - 3 * child_size : 0;
        if (have_range() && next_badge_ >= floor) {
            return 0;
        }
        return aegir::ipc::make_user_badge(aegir::ipc::user_index(own_badge_), next_badge_++);
    }
    return 0x1000 + command_serial_++;
}

uint64_t ServiceKit::reserve_shell_badge()
{
    if (shell_badge_ != 0) {
        return shell_badge_;
    }
    if (have_range()) {
        /* A serial of this process's own range: a user badge for a session's
         * terminal, a plain system serial for the boot terminal's, which auth
         * gave a range past the boot launcher's command serials so the two do
         * not collide in the registry (specs/process.md). */
        uint64_t const serial = next_badge_++;
        shell_badge_ =
            aegir::ipc::is_user_badge(own_badge_)
                ? aegir::ipc::make_user_badge(aegir::ipc::user_index(own_badge_), serial)
                : serial;
    } else {
        shell_badge_ = take_badge();
    }
    return shell_badge_;
}

uint32_t ServiceKit::take_owner()
{
    return static_cast<uint32_t>(++owner_serial_);
}

std::vector<std::string> ServiceKit::split_words(std::string const &text)
{
    std::vector<std::string> words;
    if (text.empty()) {
        return words;
    }
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\0') {
            words.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return words;
}

void ServiceKit::split_environment(std::string const &text,
                                   std::vector<std::string> &storage,
                                   std::vector<char const *> &pointers)
{
    std::size_t index = 0;
    while (index < text.size()) {
        std::size_t const start = index;
        while (index < text.size() && text[index] != '\0') {
            ++index;
        }
        storage.emplace_back(text, start, index - start);
        pointers.push_back(storage.back().c_str());
        ++index; /* past the NUL */
    }
}

seL4_CPtr ServiceKit::alloc_child_mem(uint32_t bits)
{
    aegir::ipc::Consumer const service(command_mem_);
    uint64_t const request = bits;
    uint64_t answer[1] = {};
    bool cap_arrived = false;
    aegir::ipc::WordsReply const reply = service.call_transfer(
        aegir::memory::kMethodAlloc, &request, 1, 0, answer, 1, &cap_arrived);
    if (reply.error != 0 || !cap_arrived) {
        return 0;
    }
    seL4_CPtr const slot = memory().alloc_slot();
    if (slot == 0 || !aegir::ipc::take_received_cap(slot)) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode,
                          aegir::bootstrap::kSlotReceiveCap,
                          endpoint_depth());
        return 0;
    }
    return slot;
}

bool ServiceKit::reserve_child(uint64_t *base, uint64_t *badge)
{
    uint64_t const child_size = child_quarter(badge_size_);
    if (child_size == 0 || child_count_ >= 3) {
        return false;
    }
    *base = badge_base_ + badge_size_ - (child_count_ + 1) * child_size;
    ++child_count_;
    *badge = aegir::ipc::make_user_badge(aegir::ipc::user_index(own_badge_), *base);
    return true;
}

bool ServiceKit::start_command(Command const &command, Started *out)
{
    if (!ready_ || command.words == nullptr || command.words->empty() ||
        command.cwd == nullptr) {
        return false;
    }
    std::string const &name = (*command.words)[0];
    if (!load_program(name, command.path, command.cwd)) {
        aegir::debug_write("  spawn: no image for the command ");
        aegir::debug_write(name.c_str());
        aegir::debug_write("\n");
        return false;
    }

    /* The command's id: its memory owner and its stream key. A user badge is
     * minted from the delegated range (specs/launch.md), so the memory service
     * resolves the command's class and limits (specs/memory.md, specs/limits.md)
     * and a nested terminal's commands never share a serial with its parent's;
     * a system badge keeps a small system serial, unlimited like its parent. */
    uint64_t const badge = take_badge();
    if (badge == 0) {
        aegir::debug_write("  spawn: FAIL the badge range is spent\n");
        return false;
    }
    uint32_t const owner = take_owner();
    if (!begin(owner)) {
        aegir::debug_write("  spawn: FAIL the command's staging would not begin\n");
        return false;
    }
    if (!begin_command(badge)) {
        aegir::debug_write("  spawn: FAIL no memory copy for the command\n");
        abandon(badge, owner);
        return false;
    }
    aegir::mem::Account account{"command", 0, 0, 0};
    seL4_Error untyped_error = seL4_NoError;
    uint64_t command_untyped_physical = 0;
    seL4_CPtr const command_untyped = memory().carve_untyped(
        kCommandUntypedBits, account, &untyped_error, &command_untyped_physical);
    if (command_untyped == 0) {
        aegir::debug_write("  spawn: FAIL no untyped for the command's runtime\n");
        abandon(badge, owner);
        return false;
    }

    /* `Request.arguments` is what follows argv[0]: the spawner writes
     * `request.name` as argv[0] itself (specs/environment.md). */
    std::vector<char const *> argument_pointers;
    for (std::size_t i = 1; i < command.words->size(); ++i) {
        argument_pointers.push_back((*command.words)[i].c_str());
    }
    std::vector<std::string> environment_storage;
    std::vector<char const *> environment_pointers;
    if (command.environment != nullptr) {
        split_environment(*command.environment, environment_storage, environment_pointers);
    }

    static char const kAccountText[] = "command";
    aegir::spawn::Child child{};
    child.badge = badge;
    child.runtime = command_untyped;
    child.runtime_bits = kCommandUntypedBits;
    child.mem = command_mem_;
    child.stream_badge = command.stream_badge != 0 ? command.stream_badge : badge;
    child.stream_copy = command.stream_copy;
    /* The command's con.stream is the caller's, so the kit's stream is set for
     * this spawn alone and put back after: the kit is the process's, and two
     * clients' streams must not be confused. */
    seL4_CPtr const saved_stream = kit_.stream;
    kit_.stream = command.stream;
    /* Sized to the grant builders' maximum, not to what one child happens to
     * need: a builder counts a port it could not write (kit.cc's `put` advances
     * its index either way), so a too-small array makes the count name a slot
     * past the array and the spawner reads uninitialized memory. */
    aegir::spawn::PortGrant ports[16];
    uint32_t port_count = 0;
    if (command.output_view) {
        port_count = aegir::spawn::output_ports(kit_, child, ports, 16);
    } else if (command.serve) {
        port_count = aegir::spawn::serve_ports(kit_, child, ports, 16);
    } else {
        port_count = aegir::spawn::command_ports(kit_, child, ports, 16);
    }
    kit_.stream = saved_stream;

    aegir::spawn::Request request{};
    request.name = name.c_str();
    request.name_length = static_cast<uint32_t>(name.size());
    request.binary_image = image_.data();
    request.binary_image_bytes = image_.size();
    /* The bytes are the ELF's headers only; the segments are read straight into
     * the child's frames from the still-open image (Request::image_source). */
    request.image_source = &ServiceKit::fetch_image;
    request.image_context = this;
    request.image_frame_source = &ServiceKit::fetch_image_frame;
    request.image_frame_context = this;
    request.image_size = image_size_;
    request.account = kAccountText;
    request.account_length = sizeof(kAccountText) - 1;
    request.arguments = argument_pointers.empty() ? nullptr : argument_pointers.data();
    request.argument_count = static_cast<uint32_t>(argument_pointers.size());
    request.environment =
        environment_pointers.empty() ? nullptr : environment_pointers.data();
    request.environment_count = static_cast<uint32_t>(environment_pointers.size());
    request.cwd = command.cwd->c_str();
    request.cwd_length = static_cast<uint32_t>(command.cwd->size());
    std::string const program_dir = program_directory(image_path_);
    request.program_dir = program_dir.empty() ? nullptr : program_dir.c_str();
    request.program_dir_length = static_cast<uint32_t>(program_dir.size());
    request.std_in = command.std_in != nullptr && !command.std_in->empty()
                         ? command.std_in->c_str()
                         : nullptr;
    request.std_in_length =
        request.std_in != nullptr ? static_cast<uint32_t>(command.std_in->size()) : 0;
    request.std_out = command.std_out != nullptr && !command.std_out->empty()
                          ? command.std_out->c_str()
                          : nullptr;
    request.std_out_length =
        request.std_out != nullptr ? static_cast<uint32_t>(command.std_out->size()) : 0;
    request.priority = seL4_MaxPrio - 2;
    request.ports = ports;
    request.port_count = port_count;
    request.fault_endpoint = fault_endpoint_;
    request.badge = badge;
    request.give_vspace = true;
    request.untyped_physical = command_untyped_physical;
    request.untyped_bits = kCommandUntypedBits;
    /* The launch request's stack ask (specs/launch.md): zero is the spawner's
     * default, which is what a command gets unless the launcher was asked for
     * more. */
    request.stack_pages = command.stack_pages;

    aegir::spawn::Process process{};
    if (!spawner().spawn(request, account, process)) {
        aegir::debug_write("  spawn: FAIL spawning a command: ");
        aegir::debug_write(spawner().problem());
        char const *const detail = spawner().detail();
        if (detail != nullptr && detail[0] != '\0') {
            aegir::debug_write(" (");
            aegir::debug_write(detail);
            aegir::debug_write(")");
        }
        aegir::debug_write("\n");
        abandon(badge, owner);
        return false;
    }
    /* The staging is done with: the command is alive, its capabilities are in
     * the pool owned by `owner`, and the window is free for the next command --
     * which is what lets a background `Run` and the foreground line coexist
     * (specs/memory.md Phase 5). */
    end_staging();
    /* The command is in the live set (specs/process.md): the launcher is its
     * spawner, so the launcher registers it. */
    register_child(badge, name, image_path_);
    live_.push_back(Started{process, badge, owner, command.background});
    if (out != nullptr) {
        *out = live_.back();
    }
    return true;
}

bool ServiceKit::start_output_view(Started *out)
{
    if (output_view_endpoint_ != 0) {
        return true; /* one view per launcher (specs/launch.md) */
    }
    if (allocator_ == nullptr) {
        return false;
    }
    /* The endpoint the view owns and a stream-less command calls: retyped from
     * this process's own memory, so it is the launcher's to mint each command's
     * copy from, and the view's to serve. */
    aegir::mem::Account account{"output-view", 0, 0, 0};
    seL4_Error error = seL4_NoError;
    seL4_CPtr const endpoint = allocator_->alloc_object(
        seL4_EndpointObject, seL4_EndpointBits, account, &error);
    if (endpoint == 0) {
        return false;
    }
    /* A bare name: the launcher resolves it across the session's Path (`C:`),
     * where `make_disk.py` packed it. The context is empty -- a view has no
     * current directory or environment of its own. */
    std::vector<std::string> const words{"output"};
    static std::string const empty;
    Command command;
    command.words = &words;
    command.cwd = &empty;
    command.stream = endpoint;
    command.stream_copy = true; /* the view is the receiver: a copy, all rights */
    command.output_view = true;
    if (!start_command(command, out)) {
        return false;
    }
    output_view_endpoint_ = endpoint;
    return true;
}

bool ServiceKit::start_pipeline(Stage const *stages, uint32_t count, Command const &context,
                                Started *out, uint32_t out_capacity)
{
    if (stages == nullptr || count == 0) {
        return false;
    }
    uint64_t const pipe_serial = ++pipeline_serial_;
    std::vector<Started> started;
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; ++i) {
        std::vector<std::string> const words = split_words(stages[i].line);
        if (words.empty()) {
            ok = false;
            break;
        }
        /* The ends keep the stage's own redirection; the middle is the pipe
         * that connects it to its neighbour. */
        std::string const std_in =
            i == 0 ? stages[i].std_in : pipe_path(pipe_serial, i - 1);
        std::string const std_out =
            i + 1 == count ? stages[i].std_out : pipe_path(pipe_serial, i);
        Command command = context;
        command.words = &words;
        command.std_in = &std_in;
        command.std_out = &std_out;
        Started record{};
        ok = start_command(command, &record);
        if (ok) {
            if (started.size() < out_capacity && out != nullptr) {
                out[started.size()] = record;
            }
            started.push_back(record);
        }
    }
    if (!ok) {
        /* The line would not start: take back the stages that did, so the
         * caller's pool returns whole (specs/memory.md Phase 5). */
        for (Started const &record : started) {
            release(record.badge);
        }
    }
    return ok;
}

bool ServiceKit::start_launcher(std::string const &program, std::string const &window,
                                std::vector<std::string> const &arguments,
                                std::string const &default_window, std::string const &cwd,
                                Started *out)
{
    if (!ready_ || !can_launch() || !have_range()) {
        aegir::debug_write("  spawn: no launcher kit for a nested terminal\n");
        return false;
    }
    uint64_t child_base = 0;
    uint64_t child_badge = 0;
    if (!reserve_child(&child_base, &child_badge)) {
        aegir::debug_write("  spawn: the badge range is spent\n");
        return false;
    }
    constexpr uint32_t kChildUntypedBits = 22;
    if (!load_image("Initrd:" + program)) {
        aegir::debug_write("  spawn: no image for the nested terminal\n");
        return false;
    }
    uint32_t const owner = take_owner();
    /* The staging and the child's runtime are charged to this process's own
     * badge, not the child's: the session's reclaim releases this process's
     * badge (specs/auth.md), so a nested terminal's memory comes back at logout
     * even though a nested terminal is never reaped. */
    if (!begin(owner) || !begin_command(own_badge_)) {
        aegir::debug_write("  spawn: FAIL the nested terminal's staging would not begin\n");
        abandon(own_badge_, owner);
        return false;
    }
    seL4_CPtr const child_runtime = alloc_child_mem(kChildUntypedBits);
    seL4_CPtr const child_shell_pool = alloc_child_mem(kChildUntypedBits);
    if (child_runtime == 0 || child_shell_pool == 0) {
        aegir::debug_write("  spawn: FAIL no memory for the nested terminal\n");
        abandon(own_badge_, owner);
        return false;
    }
    /* The nested terminal's kit, from the one first-class builder
     * (specs/launch.md): its own console and badge, its runtime and shell pool,
     * and the unbadged sources it hands its own commands. It is not a launcher
     * of launchers -- depth one -- so it gets no spawn:console.gui yet. */
    aegir::spawn::Child child{};
    child.badge = child_badge;
    child.runtime = child_runtime;
    child.runtime_bits = kChildUntypedBits;
    child.shell_pool = child_shell_pool;
    child.shell_pool_bits = kChildUntypedBits;
    child.launcher = false;
    /* launcher_ports fills up to 16 grants and a nested terminal appends its
     * launch half; size past both. */
    aegir::spawn::PortGrant ports[24];
    uint32_t port_count = aegir::spawn::launcher_ports(kit_, child, ports, 24);
    /* The nested terminal launches programs too (specs/launch.md): it is a
     * launcher client, so it is handed the same caller half a shell gets,
     * copied -- it is already badged. A launcher with no caller half of its own
     * hands none, and the child's shell launches nothing. */
    if (kit_.launch != 0 && port_count < 16) {
        ports[port_count] = {aegir::launch::kPortName, aegir::launch::kPortNameLength,
                             aegir::bootstrap::kSlotFirstDeclared + port_count, kit_.launch,
                             seL4_CapRights_new(1, 1, 0, 1), 0, 0, false, true};
        ++port_count;
    }

    /* The child's environment: this process's, less the launcher entries it
     * must not inherit, plus its own range and its window. */
    std::vector<std::string> environment;
    for (char const *const *e = aegir::environment::environ(); *e != nullptr; ++e) {
        std::string const entry(*e);
        if (entry.rfind("AEGIR_BADGE_RANGE=", 0) == 0 ||
            entry.rfind("AEGIR_WINDOW=", 0) == 0 || entry.rfind("AEGIR_FROM=", 0) == 0) {
            continue;
        }
        environment.push_back(entry);
    }
    std::string range_entry("AEGIR_BADGE_RANGE=");
    range_entry.append(std::to_string(child_base + 1));
    range_entry.push_back(',');
    range_entry.append(std::to_string(child_quarter(badge_size_) - 1));
    environment.push_back(std::move(range_entry));
    std::string window_entry("AEGIR_WINDOW=");
    window_entry.append(window.empty() ? default_window : window);
    environment.push_back(std::move(window_entry));
    std::vector<char const *> environment_pointers;
    environment_pointers.reserve(environment.size());
    for (std::string const &entry : environment) {
        environment_pointers.push_back(entry.c_str());
    }
    std::vector<char const *> argument_pointers;
    argument_pointers.reserve(arguments.size());
    for (std::string const &entry : arguments) {
        argument_pointers.push_back(entry.c_str());
    }

    static char const kName[] = "session.terminal";
    static char const kAccount[] = "terminal";
    aegir::spawn::Request request{};
    request.name = kName;
    request.name_length = sizeof(kName) - 1;
    request.binary_image = image_.data();
    request.binary_image_bytes = image_.size();
    /* The bytes are the ELF's headers only; the segments are read straight into
     * the child's frames from the still-open image (Request::image_source). */
    request.image_source = &ServiceKit::fetch_image;
    request.image_context = this;
    request.image_frame_source = &ServiceKit::fetch_image_frame;
    request.image_frame_context = this;
    request.image_size = image_size_;
    request.account = kAccount;
    request.account_length = sizeof(kAccount) - 1;
    request.cwd = cwd.c_str();
    request.cwd_length = static_cast<uint32_t>(cwd.size());
    request.environment = environment_pointers.data();
    request.environment_count = static_cast<uint32_t>(environment_pointers.size());
    request.arguments = argument_pointers.empty() ? nullptr : argument_pointers.data();
    request.argument_count = static_cast<uint32_t>(argument_pointers.size());
    request.priority = seL4_MaxPrio - 2;
    request.ports = ports;
    request.port_count = port_count;
    request.fault_endpoint = fault_endpoint_;
    request.badge = child_badge;
    request.give_vspace = true;
    /* A peer is a launcher too, so it gets a two-level CSpace
     * (specs/memory.md): nesting works at any depth, with the l1/l2 this
     * process uses when it is two-level, or the same shape otherwise. */
    request.cnode_bits = cnode_l2_ != 0 ? static_cast<uint32_t>(cnode_l2_) : 12;
    request.cspace_l1_bits = cnode_l1_ != 0 ? static_cast<uint32_t>(cnode_l1_) : 8;
    request.untyped_physical = 0;
    request.untyped_bits = kChildUntypedBits;
    aegir::mem::Account account{"terminal", 0, 0, 0};
    aegir::spawn::Process process{};
    if (!spawner().spawn(request, account, process)) {
        aegir::debug_write("  spawn: FAIL spawning a nested terminal: ");
        aegir::debug_write(spawner().problem());
        char const *const detail = spawner().detail();
        if (detail != nullptr && detail[0] != '\0') {
            aegir::debug_write(" (");
            aegir::debug_write(detail);
            aegir::debug_write(")");
        }
        aegir::debug_write("\n");
        abandon(own_badge_, owner);
        return false;
    }
    end_staging();
    /* The nested terminal is in the live set (specs/process.md): this process is
     * its spawner, and `program` is the Initrd image it was asked for. */
    register_child(child_badge, std::string(kName), program);
    /* The nested terminal's staging was charged to this process's own badge (a
     * nested terminal is never reaped), so its frames stay mapped for the
     * session: move the staging mark past them, or a later command's staging
     * would map onto the same addresses and the kernel would refuse it
     * (specs/memory.md Phase 5). */
    scratch_mark_ = scratch_->next();
    staged_since_rewind_ = true;
    /* A nested terminal is never reaped by badge -- it lives as long as the
     * session, and the session's reclaim takes its memory (specs/auth.md) -- so
     * it is not recorded as live. */
    if (out != nullptr) {
        *out = Started{process, child_badge, owner};
    }
    return true;
}

ServiceKit::Started *ServiceKit::find_live(uint64_t badge)
{
    for (Started &record : live_) {
        if (record.badge == badge) {
            return &record;
        }
    }
    return nullptr;
}

int ServiceKit::release(uint64_t badge)
{
    for (std::size_t i = 0; i < live_.size(); ++i) {
        if (live_[i].badge != badge) {
            continue;
        }
        bool const background = live_[i].background;
        /* The live set loses it before its memory does (specs/process.md): the
         * registry drops the row and returns the break-source slot while the
         * source capability is still valid, so a later Break never signals a
         * capability the reap has just deleted. */
        unregister_child(live_[i].badge);
        reap(live_[i].process.tcb, live_[i].badge, live_[i].owner);
        live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
        if (live_.empty()) {
            rewind_staging();
        }
        return background ? 2 : 1;
    }
    return 0;
}

}  // namespace aegir::spawn
