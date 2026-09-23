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

namespace {

/* A path and an attribute name as two strings in sequence, for the metadata
 * protocol. Zero when either does not fit the envelope. */
uint32_t pack_path_name(uint64_t *request, char const *path, uint32_t length,
                        char const *name, uint32_t name_length) noexcept
{
    uint32_t const path_words =
        nmspace::pack_string(request, path, length, nmspace::kPathMax);
    if (path_words == 0) {
        return 0;
    }
    uint32_t const name_words = nmspace::pack_string(request + path_words, name,
                                                     name_length,
                                                     metadata::kAttrNameMax);
    return name_words == 0 ? 0 : path_words + name_words;
}

}  // namespace

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
    out.mtime = answer[2];
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

bool Volume::rename(char const *src, uint32_t src_length, char const *dst,
                    uint32_t dst_length) noexcept
{
    /* Both strings ride in the one envelope: what is left after the source,
     * less its length word, bounds the destination. */
    uint64_t request[aegir::ipc::kMaxWords];
    uint32_t const src_words =
        nmspace::pack_string(request, src, src_length, nmspace::kPathMax);
    if (src_words == 0 || src_words > aegir::ipc::kMaxWords) {
        return false;
    }
    uint32_t const room = aegir::ipc::kMaxWords - src_words;
    uint32_t const dst_words =
        nmspace::pack_string(request + src_words, dst, dst_length,
                             room != 0 ? (room - 1) * 8 : 0);
    if (dst_words == 0) {
        return false;
    }
    uint64_t answer[1];
    aegir::ipc::WordsReply const reply = port_.call_words(
        volume::kMethodRename, request, src_words + dst_words, answer, 1);
    return reply.error == 0 && reply.count == 1 && answer[0] == 1;
}

bool Volume::truncate(char const *path, uint32_t length, uint64_t size) noexcept
{
    uint64_t request[nmspace::kPathMax / 8 + 2];
    uint32_t const path_words =
        nmspace::pack_string(request, path, length, nmspace::kPathMax);
    if (path_words == 0 || path_words + 1 > aegir::ipc::kMaxWords) {
        return false;
    }
    request[path_words] = size;
    uint64_t answer[1];
    aegir::ipc::WordsReply const reply = port_.call_words(
        volume::kMethodTruncate, request, path_words + 1, answer, 1);
    return reply.error == 0 && reply.count == 1 && answer[0] == 1;
}

uint64_t Volume::attr_stat(char const *path, uint32_t length, char const *name,
                           uint32_t name_length, uint32_t &type,
                           uint64_t &size) noexcept
{
    uint64_t request[aegir::ipc::kMaxWords];
    uint32_t const words = pack_path_name(request, path, length, name, name_length);
    if (words == 0) {
        return metadata::kNotFound;
    }
    uint64_t answer[metadata::kAttrStatTailWords + 1] = {};
    aegir::ipc::WordsReply const reply =
        port_.call_words(metadata::kMethodAttrStat, request, words, answer,
                         metadata::kAttrStatTailWords + 1);
    if (reply.error != 0 || reply.count < 1) {
        return metadata::kNotFound;
    }
    if (answer[0] == metadata::kOk) {
        type = static_cast<uint32_t>(answer[1]);
        size = answer[2];
    }
    return answer[0];
}

uint64_t Volume::attr_read(char const *path, uint32_t length, char const *name,
                           uint32_t name_length, uint64_t offset, void *data,
                           uint32_t &read_length) noexcept
{
    uint64_t request[aegir::ipc::kMaxWords];
    uint32_t words = pack_path_name(request, path, length, name, name_length);
    if (words == 0 || words + 2 > aegir::ipc::kMaxWords) {
        return metadata::kNotFound;
    }
    uint64_t const wanted =
        read_length < metadata::kAttrDataMax ? read_length : metadata::kAttrDataMax;
    request[words++] = offset;
    request[words++] = wanted;
    uint64_t answer[metadata::kAttrReadHeaderWords + metadata::kAttrDataMax / 8] = {};
    aegir::ipc::WordsReply const reply = port_.call_words(
        metadata::kMethodAttrRead, request, words, answer,
        metadata::kAttrReadHeaderWords + metadata::kAttrDataMax / 8);
    if (reply.error != 0 || reply.count < 1) {
        return metadata::kNotFound;
    }
    if (answer[0] == metadata::kOk) {
        uint64_t const count = answer[1];
        if (count > wanted ||
            reply.count < metadata::kAttrReadHeaderWords + (count + 7) / 8) {
            return metadata::kNotFound;
        }
        auto *out = static_cast<uint8_t *>(data);
        auto const *packed = reinterpret_cast<uint8_t const *>(
            answer + metadata::kAttrReadHeaderWords);
        for (uint64_t i = 0; i < count; ++i) {
            out[i] = packed[i];
        }
        read_length = static_cast<uint32_t>(count);
    }
    return answer[0];
}

uint64_t Volume::attr_write(char const *path, uint32_t length,
                            char const *name, uint32_t name_length,
                            uint32_t type, uint64_t offset, void const *data,
                            uint32_t data_length) noexcept
{
    if (data_length > metadata::kAttrDataMax) {
        return metadata::kNoSpace;
    }
    uint64_t request[aegir::ipc::kMaxWords];
    uint32_t words = pack_path_name(request, path, length, name, name_length);
    if (words == 0) {
        return metadata::kNotFound;
    }
    uint32_t const data_words = (data_length + 7) / 8;
    if (words + 3 + data_words > aegir::ipc::kMaxWords) {
        return metadata::kNoSpace;
    }
    request[words++] = type;
    request[words++] = offset;
    request[words++] = data_length;
    auto *packed = reinterpret_cast<uint8_t *>(request + words);
    auto const *source = static_cast<uint8_t const *>(data);
    for (uint32_t i = 0; i < data_length; ++i) {
        packed[i] = source[i];
    }
    words += data_words;
    uint64_t answer[metadata::kAttrWriteTailWords] = {};
    aegir::ipc::WordsReply const reply =
        port_.call_words(metadata::kMethodAttrWrite, request, words, answer,
                         metadata::kAttrWriteTailWords);
    if (reply.error != 0 || reply.count < 1) {
        return metadata::kNotFound;
    }
    return answer[0];
}

uint64_t Volume::attr_remove(char const *path, uint32_t length,
                             char const *name, uint32_t name_length) noexcept
{
    uint64_t request[aegir::ipc::kMaxWords];
    uint32_t const words = pack_path_name(request, path, length, name, name_length);
    if (words == 0) {
        return metadata::kNotFound;
    }
    uint64_t answer[1] = {};
    aegir::ipc::WordsReply const reply =
        port_.call_words(metadata::kMethodAttrRemove, request, words, answer, 1);
    if (reply.error != 0 || reply.count < 1) {
        return metadata::kNotFound;
    }
    return answer[0];
}

uint64_t Volume::attr_list(char const *path, uint32_t length, uint64_t index,
                           char *name, uint32_t &name_length, uint32_t &type,
                           uint64_t &size) noexcept
{
    uint64_t request[aegir::ipc::kMaxWords];
    uint32_t words =
        nmspace::pack_string(request, path, length, nmspace::kPathMax);
    if (words == 0 || words + 1 > aegir::ipc::kMaxWords) {
        return metadata::kNotFound;
    }
    request[words++] = index;
    uint64_t answer[aegir::ipc::kMaxWords] = {};
    aegir::ipc::WordsReply const reply =
        port_.call_words(metadata::kMethodAttrList, request, words, answer,
                         aegir::ipc::kMaxWords);
    if (reply.error != 0 || reply.count < 1) {
        return metadata::kNotFound;
    }
    if (answer[0] != metadata::kOk) {
        return answer[0];
    }
    char const *seen = nullptr;
    uint32_t seen_length = 0;
    if (!nmspace::unpack_string(answer + 1, reply.count - 1, metadata::kAttrNameMax,
                                &seen, &seen_length)) {
        return metadata::kNotFound;
    }
    uint32_t const tail = 2 + (seen_length + 7) / 8;
    if (reply.count < tail + metadata::kAttrListTailWords) {
        return metadata::kNotFound;
    }
    for (uint32_t i = 0; i < seen_length; ++i) {
        name[i] = seen[i];
    }
    name_length = seen_length;
    type = static_cast<uint32_t>(answer[tail]);
    size = answer[tail + 1];
    return metadata::kOk;
}

}  // namespace aegir::vfs
