/*
 * The 9P2000.L wire: the messages a client and a server exchange, and
 * nothing about how they travel.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Every 9P message begins with the same seven bytes -- size[4] type[1]
 * tag[2], little-endian, the size counting itself -- and every field after
 * them is little-endian too. A message type is either a request (Tversion,
 * 100) or the reply to one (Rversion, 101), which is always the request's
 * number plus one; the tag is the client's correlation number, and NOTAG
 * (0xffff) is the one the version handshake uses because no session exists
 * yet to number. A string is a 16-bit byte count then that many bytes, and no
 * terminator -- a *name* and a *path* differ only in convention.
 *
 * This header is the codec: writing a T-message into a buffer, reading an
 * R-message back out of one. It knows nothing about fids, files or the
 * filesystem; those live in the client (aegir-9p's consumers), and the
 * transport that carries the two buffers is a third thing again. That is what
 * makes "any 9P filesystem" a codec plus a transport rather than a filesystem
 * (specs/9p.md).
 *
 * The dialect is 9P2000.L, the one Linux's v9fs speaks and QEMU's virtio-9p
 * exports: it drops stat/wstat for getattr/setattr and adds readdir, mkdir,
 * unlinkat and renameat. The type numbers below are that dialect's; the reply
 * is always one past its request, so a reader checks `type == kRversion` and
 * not a range.
 */

#pragma once

#include <stdint.h>

namespace aegir::p9 {

/** The common header, in bytes: size[4] type[1] tag[2]. */
constexpr uint32_t kHeaderBytes = 7;

/** The tag a version handshake carries: no session exists yet to number. */
constexpr uint16_t kNoTag = 0xffff;

/** The message types, requests and replies both. A reply is a request plus
 *  one; the list is the dialect, not a promise about what is spoken yet. */
enum Type : uint8_t {
    kTlerror = 6,
    kRlerror = 7,
    kTstatfs = 8,
    kRstatfs = 9,
    kTlopen = 12,
    kRlopen = 13,
    kTlcreate = 14,
    kRlcreate = 15,
    kTsymlink = 16,
    kRsymlink = 17,
    kTmknod = 18,
    kRmknod = 19,
    kTrename = 20,
    kRrename = 21,
    kTreadlink = 22,
    kRreadlink = 23,
    kTgetattr = 24,
    kRgetattr = 25,
    kTsetattr = 26,
    kRsetattr = 27,
    kTreaddir = 40,
    kRreaddir = 41,
    kTfsync = 50,
    kRfsync = 51,
    kTlock = 52,
    kRlock = 53,
    kTgetlock = 54,
    kRgetlock = 55,
    kTlink = 70,
    kRlink = 71,
    kTmkdir = 72,
    kRmkdir = 73,
    kTrenameat = 74,
    kRrenameat = 75,
    kTunlinkat = 76,
    kRunlinkat = 77,
    kTversion = 100,
    kRversion = 101,
    kTauth = 102,
    kRauth = 103,
    kTattach = 104,
    kRattach = 105,
    kTerror = 106,
    kRerror = 107,
    kTflush = 108,
    kRflush = 109,
    kTwalk = 110,
    kRwalk = 111,
    kTread = 116,
    kRread = 117,
    kTwrite = 118,
    kRwrite = 119,
    kTclunk = 120,
    kRclunk = 121,
    kTremove = 122,
    kRremove = 123,
};

/** The qid type bits (the low byte of a qid): what kind of thing a qid names.
 *  `kQtdir` is the one the volume protocol's list and stat care about. */
constexpr uint8_t kQtdir = 0x80;
constexpr uint8_t kQtappend = 0x40;
constexpr uint8_t kQtexcl = 0x20;
constexpr uint8_t kQtsymlink = 0x02;
constexpr uint8_t kQtfile = 0x00;

/** A qid on the wire: type[1] version[4] path[8]. */
constexpr uint32_t kQidBytes = 13;

/** The longest version string a handshake reads or writes. */
constexpr uint32_t kVersionMax = 16;

/** The version string this client asks for. */
constexpr char kVersionL[] = "9P2000.L";
constexpr uint32_t kVersionLLength = sizeof(kVersionL) - 1;

/** The open flags a Tlopen carries (Linux O_*): a file writes with O_WRONLY,
 *  or O_RDWR; O_TRUNC truncates, O_CREAT is only meaningful in Tlcreate. */
constexpr uint32_t kORead = 0x0;
constexpr uint32_t kOWrite = 0x1;
constexpr uint32_t kORdwr = 0x2;
constexpr uint32_t kOTrunc = 0x200;

/** A Writer fills a buffer with a message. `begin` reserves the header; the
 *  body follows; `finish` patches the size the header promised. Any overflow
 *  sets the writer not-ok, and every later write is a no-op, so a caller may
 *  build a message straight through and check `ok()` once at the end. */
class Writer {
public:
    Writer(uint8_t *data, uint32_t capacity) noexcept
        : data_(data), capacity_(capacity) {}

    /** Start a message: the type and tag now, the size at `finish`. */
    bool begin(uint8_t type, uint16_t tag) noexcept
    {
        if (capacity_ < kHeaderBytes) {
            okay_ = false;
            return false;
        }
        data_[4] = type;
        data_[5] = static_cast<uint8_t>(tag);
        data_[6] = static_cast<uint8_t>(tag >> 8);
        offset_ = kHeaderBytes;
        okay_ = true;
        return true;
    }

    bool put_u8(uint8_t value) noexcept
    {
        if (!okay_ || offset_ + 1 > capacity_) {
            okay_ = false;
            return false;
        }
        data_[offset_++] = value;
        return true;
    }

    bool put_u16(uint16_t value) noexcept
    {
        return put_u8(static_cast<uint8_t>(value)) &&
               put_u8(static_cast<uint8_t>(value >> 8));
    }

    bool put_u32(uint32_t value) noexcept
    {
        return put_u16(static_cast<uint16_t>(value)) &&
               put_u16(static_cast<uint16_t>(value >> 16));
    }

    bool put_u64(uint64_t value) noexcept
    {
        return put_u32(static_cast<uint32_t>(value)) &&
               put_u32(static_cast<uint32_t>(value >> 32));
    }

    bool put_bytes(void const *src, uint32_t count) noexcept
    {
        if (!okay_ || count > capacity_ - offset_) {
            okay_ = false;
            return false;
        }
        auto const *bytes = static_cast<uint8_t const *>(src);
        for (uint32_t i = 0; i < count; ++i) {
            data_[offset_ + i] = bytes[i];
        }
        offset_ += count;
        return true;
    }

    /** A 9P string: a 16-bit length then the bytes, no terminator. */
    bool put_string(char const *text, uint32_t length) noexcept
    {
        return put_u16(static_cast<uint16_t>(length)) && put_bytes(text, length);
    }

    uint32_t size() const noexcept { return offset_; }
    bool ok() const noexcept { return okay_; }

    /** Patch the size field and answer whether the message is whole. */
    bool finish() noexcept
    {
        if (!okay_ || offset_ < kHeaderBytes) {
            okay_ = false;
            return false;
        }
        put_u32_at(0, offset_);
        return okay_;
    }

private:
    void put_u32_at(uint32_t at, uint32_t value) noexcept
    {
        data_[at + 0] = static_cast<uint8_t>(value);
        data_[at + 1] = static_cast<uint8_t>(value >> 8);
        data_[at + 2] = static_cast<uint8_t>(value >> 16);
        data_[at + 3] = static_cast<uint8_t>(value >> 24);
    }

    uint8_t *data_;
    uint32_t capacity_;
    uint32_t offset_ = 0;
    bool okay_ = false;
};

/** A Reader pulls fields out of a message. `head` checks the header and binds
 *  the reader to the size the message claims -- a reply that lies about its
 *  length is refused there, not read past. Every getter clears `ok()` on
 *  underflow and answers a zero from then on, so a caller may read a whole
 *  reply and check `ok()` once. */
class Reader {
public:
    Reader(uint8_t const *data, uint32_t length) noexcept
        : data_(data), length_(length) {}

    bool head(uint8_t *type, uint16_t *tag) noexcept
    {
        if (length_ < kHeaderBytes) {
            okay_ = false;
            return false;
        }
        uint32_t const size = u32_at(0);
        if (size < kHeaderBytes || size > length_) {
            okay_ = false;
            return false;
        }
        *type = data_[4];
        *tag = static_cast<uint16_t>(data_[5] | (data_[6] << 8));
        length_ = size;
        offset_ = kHeaderBytes;
        okay_ = true;
        return true;
    }

    /** A reader over a buffer with no message header -- a dirent blob from a
     *  readdir answer, an xattr value -- positioned at its first byte. `head`
     *  is for whole messages; this is for the payload inside one. */
    static Reader body(uint8_t const *data, uint32_t length) noexcept
    {
        Reader reader(data, length);
        reader.offset_ = 0;
        reader.okay_ = true;
        return reader;
    }

    bool ok() const noexcept { return okay_; }
    uint32_t remaining() const noexcept
    {
        return okay_ && offset_ <= length_ ? length_ - offset_ : 0;
    }

    uint8_t get_u8() noexcept
    {
        if (!okay_ || offset_ + 1 > length_) {
            okay_ = false;
            return 0;
        }
        return data_[offset_++];
    }

    uint16_t get_u16() noexcept
    {
        uint16_t const low = get_u8();
        uint16_t const high = get_u8();
        return static_cast<uint16_t>(low | (high << 8));
    }

    uint32_t get_u32() noexcept
    {
        uint32_t const low = get_u16();
        uint32_t const high = get_u16();
        return low | (high << 16);
    }

    uint64_t get_u64() noexcept
    {
        uint64_t const low = get_u32();
        uint64_t const high = get_u32();
        return low | (high << 32);
    }

    uint8_t const *get_bytes(uint32_t count) noexcept
    {
        if (!okay_ || count > length_ - offset_) {
            okay_ = false;
            return nullptr;
        }
        uint8_t const *at = data_ + offset_;
        offset_ += count;
        return at;
    }

    /** A 9P string: the bytes and their length, pointing into the message. */
    bool get_string(uint8_t const **text, uint32_t *length) noexcept
    {
        uint16_t const count = get_u16();
        if (!okay_) {
            return false;
        }
        uint8_t const *at = get_bytes(count);
        if (at == nullptr) {
            return false;
        }
        *text = at;
        *length = count;
        return true;
    }

private:
    uint32_t u32_at(uint32_t at) const noexcept
    {
        return static_cast<uint32_t>(data_[at + 0]) |
               (static_cast<uint32_t>(data_[at + 1]) << 8) |
               (static_cast<uint32_t>(data_[at + 2]) << 16) |
               (static_cast<uint32_t>(data_[at + 3]) << 24);
    }

    uint8_t const *data_;
    uint32_t length_;
    uint32_t offset_ = 0;
    bool okay_ = false;
};

}  // namespace aegir::p9
