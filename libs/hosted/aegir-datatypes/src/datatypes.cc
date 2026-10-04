/*
 * The datatypes client, built (specs/datatypes.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/datatypes_client.h>

#include <aegir/bootstrap.h>
#include <aegir/environment.h>
#include <aegir/launch_client.h>
#include <aegir/nmspace.h>
#include <sel4/sel4.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace aegir::datatypes {

namespace {

/* The class from a file's extension -- the first cut (specs/datatypes.md): a
 * content-sniffing broker is the generalization, and a program that knows the
 * class names it instead. */
std::string class_from_extension(std::string_view path)
{
    std::size_t const dot = path.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    std::string ext(path.substr(dot + 1));
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    if (ext == "ilbm" || ext == "iff" || ext == "lbm") {
        return "ilbm.datatype";
    }
    if (ext == "png") {
        return "png.datatype";
    }
    if (ext == "jpg" || ext == "jpeg" || ext == "jpe") {
        return "jpeg.datatype";
    }
    return {};
}

/* The session's datatypes broker, found under `datatypes.main` through the
 * bootstrap block (specs/datatypes.md). Invalid when the session declares no
 * broker, and the caller starts its class directly. */
aegir::ipc::Consumer broker() noexcept
{
    return aegir::ipc::Consumer::find(aegir::datatypes::kBrokerPortName,
                                      aegir::datatypes::kBrokerPortNameLength);
}

/* Append a namespace-shaped string to a request, answering false when the words
 * would not fit. */
bool put_string(uint64_t *words, uint32_t &at, std::string_view text)
{
    uint32_t const packed = aegir::nmspace::pack_string(
        words + at, text.data(), static_cast<uint32_t>(text.size()), aegir::nmspace::kPathMax);
    if (packed == 0 || at + packed > aegir::ipc::kMaxWords) {
        return false;
    }
    at += packed;
    return true;
}

} // namespace

void Object::reset() noexcept
{
    /* Hand the class back before the port it serves goes: the broker reaps it,
     * so its memory and its CSpace slots return to the session instead of being
     * held for as long as the session lasts. A class the client started itself
     * (no broker) is released through the launcher. */
    if (class_badge_ != 0) {
        uint64_t const badge = class_badge_;
        uint64_t answer[1] = {};
        if (manager_.valid()) {
            (void)manager_.call_words(kMethodClose, &badge, 1, answer, 1);
        } else {
            aegir::ipc::Consumer const launcher = aegir::launch::launcher();
            if (launcher.valid()) {
                (void)launcher.call_words(aegir::launch::kMethodRelease, &badge, 1, answer, 1);
            }
        }
        class_badge_ = 0;
    }
    manager_ = aegir::ipc::Consumer();
    if (send_ != 0) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, send_,
                          aegir::bootstrap::cnode_bits());
        send_ = 0;
    }
    if (page_ != nullptr && frame_ != 0) {
        scratch_->unmap(frame_);
        page_ = nullptr;
    }
    if (frame_ != 0) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, frame_,
                          aegir::bootstrap::cnode_bits());
        frame_ = 0;
    }
    class_ = aegir::ipc::Consumer();
}

Object::~Object() { reset(); }

Object::Object(Object &&other) noexcept
    : class_(other.class_), manager_(other.manager_), class_badge_(other.class_badge_),
      allocator_(other.allocator_), scratch_(other.scratch_),
      path_(std::move(other.path_)), info_(other.info_), frame_(other.frame_),
      page_(other.page_), send_(other.send_)
{
    other.class_ = aegir::ipc::Consumer();
    other.manager_ = aegir::ipc::Consumer();
    other.class_badge_ = 0;
    other.allocator_ = nullptr;
    other.scratch_ = nullptr;
    other.info_ = Info{};
    other.frame_ = 0;
    other.page_ = nullptr;
    other.send_ = 0;
}

Object &Object::operator=(Object &&other) noexcept
{
    if (this != &other) {
        reset();
        class_ = other.class_;
        manager_ = other.manager_;
        class_badge_ = other.class_badge_;
        allocator_ = other.allocator_;
        scratch_ = other.scratch_;
        path_ = std::move(other.path_);
        info_ = other.info_;
        frame_ = other.frame_;
        page_ = other.page_;
        send_ = other.send_;
        other.class_ = aegir::ipc::Consumer();
        other.manager_ = aegir::ipc::Consumer();
        other.class_badge_ = 0;
        other.allocator_ = nullptr;
        other.scratch_ = nullptr;
        other.info_ = Info{};
        other.frame_ = 0;
        other.page_ = nullptr;
        other.send_ = 0;
    }
    return *this;
}

bool Object::dispose_object()
{
    if (class_.valid()) {
        uint64_t answer[1] = {};
        (void)class_.call_words(kMethodDispose, nullptr, 0, answer, 1);
    }
    reset();
    return true;
}

Object new_object(aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch,
                  std::string_view class_name, std::string_view path)
{
    if (path.empty()) {
        return Object{};
    }
    Object object;
    object.allocator_ = &allocator;
    object.scratch_ = &scratch;
    object.path_ = std::string(path);

    /* The class's port: this makes it; the broker, or -- without one -- the
     * launcher installs the other half in the class, and the client keeps this
     * one to call. The class runs under the caller's badge (specs/libraries.md). */
    aegir::mem::Account account{"datatypes", 0, 0, 0};
    seL4_Error error = seL4_NoError;
    seL4_CPtr const endpoint =
        allocator.alloc_object(seL4_EndpointObject, seL4_EndpointBits, account, &error);
    if (endpoint == 0) {
        return Object{};
    }
    aegir::ipc::Consumer const broker_port = broker();
    bool started = false;
    if (broker_port.valid()) {
        /* The session's broker starts the class (specs/datatypes.md): the
         * request carries the class name (empty to identify from the file), the
         * path and the caller's program directory, and the class port rides as
         * the request's one capability. */
        uint64_t request[aegir::ipc::kMaxWords] = {};
        uint32_t words = 0;
        /* The caller's own program directory: a class shipped beside this
         * binary is found before `DataTypes:` (specs/datatypes.md), and only the
         * caller knows it. */
        std::string const program_dir(aegir::environment::program_dir());
        if (put_string(request, words, class_name) && put_string(request, words, path) &&
            put_string(request, words, program_dir)) {
            uint64_t answer[2] = {};
            aegir::ipc::WordsReply const reply = broker_port.call_transfer(
                kMethodOpen, request, words, endpoint, answer, 2, nullptr);
            started = reply.error == 0 && reply.count >= 2 && answer[0] == 1;
            if (started) {
                /* The class's badge: `reset` names it to the broker to release
                 * the class when this object is done with it. */
                object.manager_ = broker_port;
                object.class_badge_ = answer[1];
            }
        }
    } else if (!class_name.empty()) {
        /* No broker: start the class directly -- the first cut. */
        std::string program("DataTypes:");
        program.append(class_name);
        uint64_t badge = 0;
        started = aegir::launch::spawn_serve(
            program.c_str(), static_cast<uint32_t>(program.size()), endpoint, &badge);
        if (started) {
            /* No broker: `reset` releases the class through the launcher. */
            object.class_badge_ = badge;
        }
    }
    if (!started) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, endpoint,
                          aegir::bootstrap::cnode_bits());
        return Object{};
    }
    object.class_ = aegir::ipc::Consumer(endpoint);

    /* info: the path; answer 1 and the Info words. */
    uint64_t request[aegir::ipc::kMaxWords] = {};
    uint32_t const words = aegir::nmspace::pack_string(
        request, path.data(), static_cast<uint32_t>(path.size()),
        aegir::nmspace::kPathMax);
    if (words == 0) {
        object.reset();
        return Object{};
    }
    uint64_t answer[1 + kInfoWords] = {};
    aegir::ipc::WordsReply const reply =
        object.class_.call_words(kMethodInfo, request, words, answer, 1 + kInfoWords);
    if (reply.error != 0 || reply.count < 1 + kInfoWords || answer[0] != 1) {
        object.reset();
        return Object{};
    }
    object.info_.width = static_cast<uint32_t>(answer[1]);
    object.info_.height = static_cast<uint32_t>(answer[2]);
    object.info_.format = static_cast<Format>(answer[3]);
    object.info_.stride = static_cast<uint32_t>(answer[4]);
    object.info_.palette_size = static_cast<uint32_t>(answer[5]);
    object.info_.transparent = (answer[6] & 1u) != 0;
    object.info_.transparent_index = static_cast<uint16_t>(answer[6] >> 16);
    return object;
}

Object new_object(aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch,
                  std::string_view path)
{
    /* The extension is the first cut's hint; the broker identifies from the
     * content, so an empty class name is still worth asking it. Without a broker
     * an empty name starts nothing. */
    return new_object(allocator, scratch, class_from_extension(path), path);
}

bool Object::read(Decoded &into)
{
    if (!class_.valid() || allocator_ == nullptr || scratch_ == nullptr) {
        return false;
    }
    if (info_.stride == 0 || info_.height == 0) {
        return false;
    }

    into.info = info_;
    into.pixels.assign(static_cast<std::size_t>(info_.stride) * info_.height, 0);
    into.palette.assign(info_.palette_size, Color{});
    std::size_t const total = into.pixels.size() + into.palette.size() * 3;
    if (total == 0) {
        return false;
    }

    /* The transfer page: one mapped frame the client reads through, and an
     * unmapped copy to hand over -- a frame capability pins to the VSpace it is
     * first mapped into, so the copy is what crosses (server_font.cc). */
    aegir::mem::Account account{"datatypes-page", 0, 0, 0};
    seL4_Error error = seL4_NoError;
    frame_ = allocator_->alloc_page(account, &error);
    if (frame_ == 0) {
        return false;
    }
    page_ = static_cast<uint8_t *>(scratch_->map(frame_));
    if (page_ == nullptr) {
        return false;
    }
    send_ = allocator_->alloc_slot();
    if (send_ == 0 ||
        seL4_CNode_Copy(aegir::bootstrap::kSlotOwnCNode, send_,
                        aegir::bootstrap::cnode_bits(), aegir::bootstrap::kSlotOwnCNode,
                        frame_, aegir::bootstrap::cnode_bits(),
                        seL4_AllRights) != seL4_NoError) {
        return false;
    }

    std::vector<uint8_t> stream(total, 0);
    std::size_t at = 0;
    bool ok = true;
    while (at < total) {
        uint64_t request[aegir::ipc::kMaxWords] = {};
        uint32_t words = aegir::nmspace::pack_string(
            request, path_.data(), static_cast<uint32_t>(path_.size()),
            aegir::nmspace::kPathMax);
        if (words == 0 || words + 1 > aegir::ipc::kMaxWords) {
            ok = false;
            break;
        }
        request[words++] = at;
        uint64_t answer[2] = {};
        aegir::ipc::WordsReply const reply = class_.call_transfer(
            kMethodRead, request, words, send_, answer, 2, nullptr);
        if (reply.error != 0 || reply.count < 2 || answer[0] != 1) {
            ok = false;
            break;
        }
        std::size_t const written = static_cast<std::size_t>(answer[1]);
        if (written == 0) {
            break;
        }
        if (written > total - at) {
            ok = false;
            break;
        }
        std::memcpy(stream.data() + at, page_, written);
        at += written;
    }

    if (page_ != nullptr) {
        scratch_->unmap(frame_);
        page_ = nullptr;
    }
    if (send_ != 0) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, send_,
                          aegir::bootstrap::cnode_bits());
        send_ = 0;
    }
    if (frame_ != 0) {
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, frame_,
                          aegir::bootstrap::cnode_bits());
        frame_ = 0;
    }
    if (!ok || at != total) {
        return false;
    }

    std::memcpy(into.pixels.data(), stream.data(), into.pixels.size());
    if (!into.palette.empty()) {
        std::memcpy(into.palette.data(), stream.data() + into.pixels.size(),
                    into.palette.size() * 3);
    }
    return true;
}

} // namespace aegir::datatypes
