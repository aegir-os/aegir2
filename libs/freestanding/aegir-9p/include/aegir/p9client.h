/*
 * A 9P2000.L client: fids, tags, and the messages a filesystem asks for.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The codec (aegir/9p.h) writes one message and reads one back; this is the
 * session on top of it. It speaks through a transport (aegir/p9transport.h),
 * which carries the two buffers and nothing else, so the same client runs over
 * virtio-9p today and a socket later (specs/9p.md).
 *
 * A 9P session is a set of *fids*: numbers the client chooses, each naming one
 * walked-and-maybe-open file on the server, until it is clunked. The client
 * owns the session and hands the caller messages; it does not model a path or
 * a handle -- the service above it does. Every call is synchronous (one
 * request in flight, which the transport's single window makes literal), so a
 * tag is only checked, never queued.
 */

#pragma once

#include <aegir/9p.h>
#include <aegir/p9transport.h>
#include <stdint.h>

namespace aegir::p9 {

/** `afid` in Tattach: no authentication fid. */
constexpr uint32_t kNoFid = 0xffffffffu;

/** The `mode` bits a qid's `type` carries (POSIX `S_IFMT`). */
constexpr uint64_t kSIfmt = 0xf000;
constexpr uint64_t kSIfdir = 0x4000;

/** One path component to walk, a slice of the caller's string. */
struct Name {
    char const *data;
    uint32_t length;
};

/** A qid: the server's identity for a file. */
struct Qid {
    uint8_t type;
    uint32_t version;
    uint64_t path;
};

class Client {
public:
    explicit Client(p9transport::Transport *transport) noexcept : transport_(transport) {}

    uint32_t msize() const noexcept { return msize_; }

    /** Negotiate the dialect and the message size. Answers the size the export
     *  will carry (the smaller of `wanted` and its own limit), or zero when the
     *  handshake failed or the dialect is not 9P2000.L. */
    uint32_t version(uint32_t wanted) noexcept
    {
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTversion, kNoTag);
        w.put_u32(wanted);
        w.put_string(kVersionL, kVersionLLength);
        Reader r(nullptr, 0);
        if (!run(w, r, kRversion, kNoTag)) {
            return 0;
        }
        uint32_t const msize = r.get_u32();
        uint8_t const *version = nullptr;
        uint32_t version_length = 0;
        r.get_string(&version, &version_length);
        if (!r.ok() || msize == 0 || version_length < kVersionLLength) {
            return 0;
        }
        for (uint32_t i = 0; i < kVersionLLength; ++i) {
            if (version[i] != static_cast<uint8_t>(kVersionL[i])) {
                return 0;
            }
        }
        msize_ = msize;
        return msize;
    }

    /** Attach the export's root to `fid`, as `uname` (an empty name is the
     *  host user, which is what `security_model=none` wants). */
    bool attach(uint32_t fid, char const *uname, uint32_t uname_length) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTattach, tag);
        w.put_u32(fid);
        w.put_u32(kNoFid);
        w.put_string(uname, uname_length);
        w.put_string("", 0);
        w.put_u32(kNoFid); /* n_uname: unknown, the host user is the identity */
        Reader r(nullptr, 0);
        if (!run(w, r, kRattach, tag)) {
            return false;
        }
        r.get_bytes(kQidBytes);
        return r.ok();
    }

    /** Walk `count` names from `fid` to `newfid`. Answers how many were walked
     *  (equal to `count` on success), or -1 on refusal. */
    int32_t walk(uint32_t fid, uint32_t newfid, Name const *names, uint32_t count,
                 Qid *qids, uint32_t capacity) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTwalk, tag);
        w.put_u32(fid);
        w.put_u32(newfid);
        w.put_u16(static_cast<uint16_t>(count));
        for (uint32_t i = 0; i < count; ++i) {
            w.put_string(names[i].data, names[i].length);
        }
        Reader r(nullptr, 0);
        if (!run(w, r, kRwalk, tag)) {
            return -1;
        }
        uint16_t const walked = r.get_u16();
        if (!r.ok()) {
            return -1;
        }
        for (uint32_t i = 0; i < walked; ++i) {
            uint8_t const *qid = r.get_bytes(kQidBytes);
            if (qid == nullptr) {
                return -1;
            }
            if (i < capacity) {
                qids[i].type = qid[0];
                qids[i].version = static_cast<uint32_t>(qid[1]) |
                                  (static_cast<uint32_t>(qid[2]) << 8) |
                                  (static_cast<uint32_t>(qid[3]) << 16) |
                                  (static_cast<uint32_t>(qid[4]) << 24);
                uint64_t path = 0;
                for (uint32_t b = 0; b < 8; ++b) {
                    path |= static_cast<uint64_t>(qid[5 + b]) << (8 * b);
                }
                qids[i].path = path;
            }
        }
        return walked;
    }

    /** Open `fid` with the POSIX flags (kORead, kOTrunc, ...). */
    bool open(uint32_t fid, uint32_t flags) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTlopen, tag);
        w.put_u32(fid);
        w.put_u32(flags);
        Reader r(nullptr, 0);
        if (!run(w, r, kRlopen, tag)) {
            return false;
        }
        r.get_bytes(kQidBytes);
        r.get_u32(); /* iounit */
        return r.ok();
    }

    /** Release `fid`. */
    bool clunk(uint32_t fid) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTclunk, tag);
        w.put_u32(fid);
        Reader r(nullptr, 0);
        return run(w, r, kRclunk, tag);
    }

    /** `valid` bits for Tsetattr: only the size is used here. */
    static constexpr uint32_t kSetattrSize = 0x8;

    /** Create `name` in the directory `fid` and open it: the fid becomes the
     *  new file's (9P reuse), so the caller keeps the same number. */
    bool create(uint32_t fid, char const *name, uint32_t name_length, uint32_t flags,
                uint32_t mode, uint32_t gid) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTlcreate, tag);
        w.put_u32(fid);
        w.put_string(name, name_length);
        w.put_u32(flags);
        w.put_u32(mode);
        w.put_u32(gid);
        Reader r(nullptr, 0);
        if (!run(w, r, kRlcreate, tag)) {
            return false;
        }
        r.get_bytes(kQidBytes);
        r.get_u32(); /* iounit */
        return r.ok();
    }

    /** A directory in `fid` named `name`. */
    bool mkdir(uint32_t fid, char const *name, uint32_t name_length, uint32_t mode,
               uint32_t gid) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTmkdir, tag);
        w.put_u32(fid);
        w.put_string(name, name_length);
        w.put_u32(mode);
        w.put_u32(gid);
        Reader r(nullptr, 0);
        if (!run(w, r, kRmkdir, tag)) {
            return false;
        }
        r.get_bytes(kQidBytes);
        return r.ok();
    }

    /** Remove the file (or, with `flags` AT_REMOVEDIR, the empty directory)
     *  `name` from the directory `fid`. */
    bool unlinkat(uint32_t fid, char const *name, uint32_t name_length,
                  uint32_t flags) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTunlinkat, tag);
        w.put_u32(fid);
        w.put_string(name, name_length);
        w.put_u32(flags);
        Reader r(nullptr, 0);
        return run(w, r, kRunlinkat, tag);
    }

    /** Rename `oldname` in `oldfid` to `newname` in `newfid`. */
    bool renameat(uint32_t oldfid, char const *oldname, uint32_t oldname_length,
                  uint32_t newfid, char const *newname, uint32_t newname_length) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTrenameat, tag);
        w.put_u32(oldfid);
        w.put_string(oldname, oldname_length);
        w.put_u32(newfid);
        w.put_string(newname, newname_length);
        Reader r(nullptr, 0);
        return run(w, r, kRrenameat, tag);
    }

    /** Set the file's size (Tsetattr with only the size bit): the tail is freed
     *  on a shrink, zeroed on a grow. */
    bool setattr_size(uint32_t fid, uint64_t size) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTsetattr, tag);
        w.put_u32(fid);
        w.put_u32(kSetattrSize);
        w.put_u32(0); /* mode */
        w.put_u32(0); /* uid */
        w.put_u32(0); /* gid */
        w.put_u64(size);
        w.put_u64(0); /* atime_sec */
        w.put_u64(0); /* atime_nsec */
        w.put_u64(0); /* mtime_sec */
        w.put_u64(0); /* mtime_nsec */
        Reader r(nullptr, 0);
        return run(w, r, kRsetattr, tag);
    }

    /** Write `count` bytes at `offset`. Answers the bytes written, or -1. */
    int32_t write(uint32_t fid, uint64_t offset, uint8_t const *data,
                  uint32_t count) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTwrite, tag);
        w.put_u32(fid);
        w.put_u64(offset);
        w.put_u32(count);
        w.put_bytes(data, count);
        Reader r(nullptr, 0);
        if (!run(w, r, kRwrite, tag)) {
            return -1;
        }
        uint32_t const written = r.get_u32();
        if (!r.ok()) {
            return -1;
        }
        return static_cast<int32_t>(written);
    }

    /** Read `count` bytes at `offset`. `data` points into the reply -- valid
     *  until the next call on this client -- and the answer is the bytes read,
     *  or -1 on refusal. */
    int32_t read(uint32_t fid, uint64_t offset, uint32_t count,
                 uint8_t const **data) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTread, tag);
        w.put_u32(fid);
        w.put_u64(offset);
        w.put_u32(count);
        Reader r(nullptr, 0);
        if (!run(w, r, kRread, tag)) {
            return -1;
        }
        uint32_t const got = r.get_u32();
        uint8_t const *bytes = r.get_bytes(got);
        if (bytes == nullptr) {
            return -1;
        }
        *data = bytes;
        return static_cast<int32_t>(got);
    }

    /** Read a directory chunk at `offset`. `data`/`length` is the raw dirent
     *  blob -- each entry {qid[13], offset[8], type[1], name[s]} -- valid until
     *  the next call. */
    bool readdir(uint32_t fid, uint64_t offset, uint32_t count, uint8_t const **data,
                 uint32_t *length) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTreaddir, tag);
        w.put_u32(fid);
        w.put_u64(offset);
        w.put_u32(count);
        Reader r(nullptr, 0);
        if (!run(w, r, kRreaddir, tag)) {
            return false;
        }
        uint32_t const got = r.get_u32();
        uint8_t const *bytes = r.get_bytes(got);
        if (bytes == nullptr) {
            return false;
        }
        *data = bytes;
        *length = got;
        return true;
    }

    /** A file's attributes, as much as the volume protocol asks for. */
    struct Attr {
        uint64_t mode;
        uint64_t size;
        uint64_t mtime; /* whole seconds; zero when the server has no clock */
        bool is_dir;
    };

    bool getattr(uint32_t fid, Attr *attr) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTgetattr, tag);
        w.put_u32(fid);
        w.put_u64(0xffffffffULL); /* every attribute the server has */
        Reader r(nullptr, 0);
        if (!run(w, r, kRgetattr, tag)) {
            return false;
        }
        r.get_u64();          /* valid */
        r.get_bytes(kQidBytes);
        uint64_t const mode = r.get_u32();
        r.get_u32();          /* uid */
        r.get_u32();          /* gid */
        r.get_u64();          /* nlink */
        r.get_u64();          /* rdev */
        uint64_t const size = r.get_u64();
        r.get_u64();          /* blksize */
        r.get_u64();          /* blocks */
        r.get_u64();          /* atime_sec */
        r.get_u64();          /* atime_nsec */
        uint64_t const mtime = r.get_u64();
        if (!r.ok()) {
            return false;
        }
        attr->mode = mode;
        attr->size = size;
        attr->mtime = mtime;
        attr->is_dir = (mode & kSIfmt) == kSIfdir;
        return true;
    }

    /** The export's capacity. */
    struct Statfs {
        uint64_t total;
        uint64_t free;
    };

    bool statfs(uint32_t fid, Statfs *out) noexcept
    {
        uint16_t const tag = next_tag();
        Writer w(transport_->request_buffer(), transport_->request_capacity());
        w.begin(kTstatfs, tag);
        w.put_u32(fid);
        Reader r(nullptr, 0);
        if (!run(w, r, kRstatfs, tag)) {
            return false;
        }
        r.get_u32(); /* type */
        uint64_t const bsize = r.get_u32();
        uint64_t const blocks = r.get_u64();
        uint64_t const bfree = r.get_u64();
        if (!r.ok()) {
            return false;
        }
        out->total = blocks * bsize;
        out->free = bfree * bsize;
        return true;
    }

private:
    /** The tag the next request carries: the client's correlation number, never
     *  NOTAG. */
    uint16_t next_tag() noexcept
    {
        ++tag_;
        if (tag_ == kNoTag) {
            tag_ = 1;
        }
        return tag_;
    }

    /** Run the request just built and bind a reader to the reply, checking the
     *  type and tag. False on any mismatch -- a refusal (Rlerror) included. */
    bool run(Writer &writer, Reader &reader, uint8_t expected, uint16_t tag) noexcept
    {
        if (!writer.finish() || !transport_->round_trip(writer.size())) {
            return false;
        }
        reader = Reader(transport_->reply(), transport_->reply_length());
        uint8_t type = 0;
        uint16_t answer_tag = 0;
        return reader.head(&type, &answer_tag) && answer_tag == tag && type == expected;
    }

    p9transport::Transport *transport_;
    uint32_t msize_ = 0;
    uint16_t tag_ = 0;
};

}  // namespace aegir::p9
