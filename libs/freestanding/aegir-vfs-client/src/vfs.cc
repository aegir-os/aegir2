/*
 * Aegir's VFS calls -- implementation. See include/aegir/vfs.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The wire shapes are the two protocol headers': a string packs as a length
 * word then its bytes (aegir/nmspace.h's pack_string), and every answer is
 * read back the same way. Nothing here is a policy the protocols do not
 * already carry -- a refusal is a false, and the bytes a read answers with are
 * the volume's, not ours.
 */

#include <aegir/vfs.h>

namespace aegir::vfs {

aegir::ipc::Consumer find_namespace() noexcept
{
    return aegir::ipc::Consumer::find(nmspace::kPortName, nmspace::kPortNameLength);
}

Namespace::Namespace(aegir::ipc::Consumer port) noexcept : port_(port), rest_{} {}

Namespace Namespace::find() noexcept
{
    return Namespace(find_namespace());
}

bool Namespace::resolve(char const *path, uint32_t length, seL4_CPtr slot,
                        Resolved &out) noexcept
{
    uint64_t request[nmspace::kPathMax / 8 + 1];
    uint32_t const request_words =
        nmspace::pack_string(request, path, length, nmspace::kPathMax);
    if (request_words == 0) {
        return false;
    }
    uint64_t answer[nmspace::kResolveWords];
    bool cap_arrived = false;
    aegir::ipc::WordsReply const reply =
        port_.call_transfer(nmspace::kMethodResolve, request, request_words, 0, answer,
                            nmspace::kResolveWords, &cap_arrived);
    char const *text = nullptr;
    uint32_t rest_length = 0;
    if (reply.error != 0 || !cap_arrived ||
        !nmspace::unpack_string(answer, reply.count, nmspace::kPathMax, &text, &rest_length) ||
        !aegir::ipc::take_received_cap(slot)) {
        return false;
    }
    for (uint32_t i = 0; i < rest_length; ++i) {
        rest_[i] = text[i];
    }
    out.volume = slot;
    out.rest = rest_;
    out.rest_length = rest_length;
    return true;
}

bool Namespace::volume_count(uint64_t &count) const noexcept
{
    uint64_t answer[1];
    aegir::ipc::WordsReply const reply =
        port_.call_words(nmspace::kMethodCount, nullptr, 0, answer, 1);
    if (reply.error != 0 || reply.count != 1) {
        return false;
    }
    count = answer[0];
    return true;
}

bool Namespace::describe(uint64_t index, nmspace::Row &row) const noexcept
{
    uint64_t answer[nmspace::kRowWords];
    aegir::ipc::WordsReply const reply =
        port_.call_words(nmspace::kMethodDescribe, &index, 1, answer, nmspace::kRowWords);
    if (reply.error != 0 || reply.count != nmspace::kRowWords) {
        return false;
    }
    /* The server packed a Row into these words; read them back as one. The
     * array is word-aligned, which is the alignment a Row asks for. */
    row = *reinterpret_cast<nmspace::Row const *>(answer);
    return true;
}

Volume::Volume(seL4_CPtr port) noexcept : port_(port), reply_{} {}

bool Volume::read(char const *path, uint32_t length, uint64_t offset, uint64_t capacity,
                  Bytes &out) noexcept
{
    uint64_t request[nmspace::kPathMax / 8 + 3];
    uint32_t request_words = nmspace::pack_string(request, path, length, nmspace::kPathMax);
    if (request_words == 0) {
        return false;
    }
    uint64_t const wanted = capacity < volume::kReadMax ? capacity : volume::kReadMax;
    request[request_words++] = offset;
    request[request_words++] = wanted;
    aegir::ipc::WordsReply const reply =
        port_.call_words(volume::kMethodRead, request, request_words, reply_,
                         aegir::ipc::kMaxWords);
    if (reply.error != 0 || reply.count < volume::kReadHeaderWords) {
        return false;
    }
    uint64_t const count = reply_[0];
    uint64_t const eof = reply_[1];
    if (count > wanted || reply.count < volume::kReadHeaderWords + (count + 7) / 8) {
        return false;
    }
    out.data = reinterpret_cast<char const *>(reply_ + volume::kReadHeaderWords);
    out.count = count;
    out.eof = eof != 0;
    return true;
}

bool Volume::list(char const *path, uint32_t length, uint64_t index, Entry &out) noexcept
{
    uint64_t request[nmspace::kPathMax / 8 + 1];
    uint32_t request_words = nmspace::pack_string(request, path, length, nmspace::kPathMax);
    if (request_words == 0) {
        return false;
    }
    request[request_words++] = index;
    aegir::ipc::WordsReply const reply =
        port_.call_words(volume::kMethodList, request, request_words, reply_,
                         aegir::ipc::kMaxWords);
    if (reply.error != 0 || reply.count == 0) {
        return false;
    }
    char const *name = nullptr;
    uint32_t name_length = 0;
    if (!nmspace::unpack_string(reply_, reply.count, nmspace::kPathMax, &name, &name_length)) {
        return false;
    }
    uint32_t const tail = 1 + (name_length + 7) / 8;
    if (reply.count < tail + volume::kListTailWords) {
        return false;
    }
    out.name = name;
    out.name_length = name_length;
    out.size = reply_[tail];
    out.kind = reply_[tail + 1];
    return true;
}

bool Volume::stat(char const *path, uint32_t length, Info &out) noexcept
{
    uint64_t request[nmspace::kPathMax / 8 + 1];
    uint32_t const request_words =
        nmspace::pack_string(request, path, length, nmspace::kPathMax);
    if (request_words == 0) {
        return false;
    }
    uint64_t answer[volume::kStatTailWords];
    aegir::ipc::WordsReply const reply =
        port_.call_words(volume::kMethodStat, request, request_words, answer,
                         volume::kStatTailWords);
    if (reply.error != 0 || reply.count != volume::kStatTailWords) {
        return false;
    }
    out.kind = answer[0];
    out.size = answer[1];
    return true;
}

uint64_t Volume::open(char const *path, uint32_t length, uint64_t flags) noexcept
{
    uint64_t request[nmspace::kPathMax / 8 + 2];
    uint32_t request_words = nmspace::pack_string(request, path, length, nmspace::kPathMax);
    if (request_words == 0) {
        return 0;
    }
    request[request_words++] = flags;
    uint64_t answer[1];
    aegir::ipc::WordsReply const reply =
        port_.call_words(volume::kMethodOpen, request, request_words, answer, 1);
    if (reply.error != 0 || reply.count != 1) {
        return 0;
    }
    return answer[0];
}

bool Volume::write(uint64_t handle, void const *bytes, uint32_t count,
                   uint64_t *written) noexcept
{
    if (count > volume::kWriteMax) {
        return false;
    }
    uint64_t request[2 + volume::kWriteMax / 8];
    request[0] = handle;
    request[1] = count;
    auto *packed = reinterpret_cast<uint8_t *>(request + 2);
    auto const *source = static_cast<uint8_t const *>(bytes);
    for (uint32_t i = 0; i < count; ++i) {
        packed[i] = source[i];
    }
    uint64_t answer[1];
    aegir::ipc::WordsReply const reply =
        port_.call_words(volume::kMethodWrite, request, 2 + (count + 7) / 8, answer, 1);
    if (reply.error != 0 || reply.count != 1) {
        return false;
    }
    if (written != nullptr) {
        *written = answer[0];
    }
    return answer[0] == count;
}

bool Volume::close(uint64_t handle) noexcept
{
    uint64_t answer[1];
    aegir::ipc::WordsReply const reply =
        port_.call_words(volume::kMethodClose, &handle, 1, answer, 1);
    return reply.error == 0 && reply.count == 1 && answer[0] == 1;
}

bool Volume::make_directory(char const *path, uint32_t length) noexcept
{
    uint64_t request[nmspace::kPathMax / 8 + 1];
    uint32_t const request_words =
        nmspace::pack_string(request, path, length, nmspace::kPathMax);
    if (request_words == 0) {
        return false;
    }
    uint64_t answer[1];
    aegir::ipc::WordsReply const reply =
        port_.call_words(volume::kMethodMkdir, request, request_words, answer, 1);
    return reply.error == 0 && reply.count == 1 && answer[0] == 1;
}

bool Volume::remove(char const *path, uint32_t length) noexcept
{
    uint64_t request[nmspace::kPathMax / 8 + 1];
    uint32_t const request_words =
        nmspace::pack_string(request, path, length, nmspace::kPathMax);
    if (request_words == 0) {
        return false;
    }
    uint64_t answer[1];
    aegir::ipc::WordsReply const reply =
        port_.call_words(volume::kMethodRemove, request, request_words, answer, 1);
    return reply.error == 0 && reply.count == 1 && answer[0] == 1;
}

}  // namespace aegir::vfs
