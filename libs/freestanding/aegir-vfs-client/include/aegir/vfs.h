/*
 * Aegir's VFS calls, in a freestanding library.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The VFS owns the namespace (volume names to capabilities, paths to a volume
 * and the rest it serves) and each filesystem owns the data behind a volume
 * port (specs/vfs.md). This wraps both wires so a caller asks for a file
 * instead of building IPC by hand. It is the transport other libraries and
 * services stand on: the hosted aegir::filesystem wrapper sits over it for the
 * std::filesystem error model, and the runtime's own file calls will too
 * (specs/cxx.md step 5).
 *
 * The library is freestanding on purpose -- seL4 and the two protocol headers,
 * nothing else -- so a freestanding service that holds `vfs.namespace` can
 * link it without the exception personality. Every call answers with a value
 * or a refusal; a hosted caller that wants std::filesystem_error wraps it.
 */

#ifndef AEGIR_VFS_H
#define AEGIR_VFS_H

#include <aegir/ipc/port.h>
#include <aegir/metadata.h>
#include <aegir/nmspace.h>
#include <aegir/volume.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace aegir::vfs {

/** The namespace port this process was given, through its bootstrap block
 *  (specs/services.md). Invalid when it holds none -- a program that needs the
 *  filesystem says so in its manifest `needs`. */
aegir::ipc::Consumer find_namespace() noexcept;

/**
 * The VFS's namespace: resolve a `Volume:rest` path to a volume capability,
 * and enumerate the volumes. One resolve buffer lives in the object, because
 * the rest an alias composes belongs to the namespace, not the caller.
 */
class Namespace {
public:
    explicit Namespace(aegir::ipc::Consumer port) noexcept;

    /** The port this process was given, if any. */
    static Namespace find() noexcept;

    bool valid() const noexcept { return port_.valid(); }

    /** Where a resolve landed: the volume's port capability, and the
     *  volume-relative rest. `rest` points into this Namespace and is valid
     *  until the next resolve. */
    struct Resolved {
        seL4_CPtr volume;
        char const *rest;
        uint32_t rest_length;
    };

    /**
     * One resolve of `path` into `slot` (a free capability in our CSpace).
     * False when the volume is not registered, the path has no volume part, or
     * the capability did not arrive. A caller still coming up -- a boot
     * service racing the volumes -- asks again; this does not wait.
     */
    bool resolve(char const *path, uint32_t length, seL4_CPtr slot, Resolved &out) noexcept;

    /** How many volumes the namespace holds, and one describe row. A Row is
     *  the name, the flags, whether a filesystem's capability is held, and the
     *  filesystem's type. */
    bool volume_count(uint64_t &count) const noexcept;
    bool describe(uint64_t index, nmspace::Row &row) const noexcept;

    /** The Row of the volume `path` resolves to, following aliases: the name
     *  and the filesystem type a caller learns without holding the volume
     *  capability (nmspace::kMethodDescribePath). False when the path resolves
     *  to no volume. */
    bool describe_path(char const *path, uint32_t length, nmspace::Row &row) const noexcept;

private:
    aegir::ipc::Consumer port_;
    char rest_[nmspace::kPathMax];
};

/**
 * One filesystem's volume port: the reads and listings that need no handle,
 * and the write side, which is handles (specs/vfs.md). The bytes a read or a
 * list answers with live in the object and are valid until the next call on
 * it.
 */
class Volume {
public:
    explicit Volume(seL4_CPtr port) noexcept;

    bool valid() const noexcept { return port_.valid(); }

    /** A read answer: the bytes and whether the file ends there. `data` points
     *  into this Volume. */
    struct Bytes {
        char const *data;
        uint64_t count;
        bool eof;
    };

    /**
     * Read up to `capacity` bytes at `offset`. One answer carries at most
     * volume::kReadMax bytes, so a larger `capacity` is clamped and the caller
     * asks again at the next offset. False when the volume refused.
     */
    bool read(char const *path, uint32_t length, uint64_t offset, uint64_t capacity,
              Bytes &out) noexcept;

    /** One directory entry: the name (into this Volume), its size and kind
     *  (volume::kKindFile or kKindDir). False at the end of the directory or
     *  on refusal -- the cursor is the caller's index. */
    struct Entry {
        char const *name;
        uint32_t name_length;
        uint64_t size;
        uint64_t kind;
    };
    bool list(char const *path, uint32_t length, uint64_t index, Entry &out) noexcept;

    /** One path's kind and size, without listing the directory it lives in
     *  (volume::kMethodStat). Kind is volume::kKindFile or kKindDir; a
     *  directory's size is zero. False when the path is not there or the
     *  volume refuses. */
    struct Info {
        uint64_t kind;
        uint64_t size;
        uint64_t mtime; /* whole seconds since the Unix epoch, 0 for none */
    };
    bool stat(char const *path, uint32_t length, Info &out) noexcept;

    /* The write side (specs/vfs.md): handles, the only per-client state a
     * filesystem holds. Zero is never a handle, and one is never reused. */
    uint64_t open(char const *path, uint32_t length, uint64_t flags) noexcept;
    bool write(uint64_t handle, void const *bytes, uint32_t count,
               uint64_t *written = nullptr) noexcept;
    bool close(uint64_t handle) noexcept;
    bool make_directory(char const *path, uint32_t length) noexcept;
    bool remove(char const *path, uint32_t length) noexcept;

    /** Rename `src` to `dst`: the entry is remade under the new name, its
     *  data unmoved (volume::kMethodRename). Both names must be in the same
     *  directory; a destination that exists and an open handle on the source
     *  are refused. False on refusal. */
    bool rename(char const *src, uint32_t src_length, char const *dst,
                uint32_t dst_length) noexcept;

    /** Cut or grow `path` to `size` bytes (volume::kMethodTruncate): the tail
     *  is freed on a shrink, zeroed clusters are added on a grow. A directory,
     *  a missing name, and a read-only volume are refused. False on refusal. */
    bool truncate(char const *path, uint32_t length, uint64_t size) noexcept;

    /* Attributes (aegir/metadata.h). Unlike the calls above, each answers the
     * protocol's status word: metadata::kOk, or kUnsupported / kNotFound /
     * kReadOnly / ... The caller tells "no such attribute" from "no metadata"
     * by it. */

    /** `name`'s type_code and size. */
    uint64_t attr_stat(char const *path, uint32_t length, char const *name,
                       uint32_t name_length, uint32_t &type,
                       uint64_t &size) noexcept;

    /** Read `name`'s value at `offset`. `length` is the room in `data` and, on
     *  return, the bytes read. A value larger than one envelope is read at
     *  successive offsets. */
    uint64_t attr_read(char const *path, uint32_t length, char const *name,
                       uint32_t name_length, uint64_t offset, void *data,
                       uint32_t &read_length) noexcept;

    /** Write `name`'s value at `offset`, making it with `type` when absent. */
    uint64_t attr_write(char const *path, uint32_t length, char const *name,
                        uint32_t name_length, uint32_t type, uint64_t offset,
                        void const *data, uint32_t data_length) noexcept;

    /** Remove `name`. */
    uint64_t attr_remove(char const *path, uint32_t length, char const *name,
                         uint32_t name_length) noexcept;

    /** The `index`th attribute's name, type and size; kNotFound past the
     *  last. */
    uint64_t attr_list(char const *path, uint32_t length, uint64_t index,
                       char *name, uint32_t &name_length, uint32_t &type,
                       uint64_t &size) noexcept;

    /* The typed layer (aegir/metadata.h's codecs): the value in a C++ shape,
     * stored with the matching type_code. A getter refuses an attribute whose
     * stored type or size is not the one asked for, so an int read as one is
     * an int. Each answers the protocol's status word. */

    /** A string's bytes, no terminator. `capacity` is the room in `out`;
     *  `out_length` is the length written. */
    uint64_t attr_get_string(char const *path, uint32_t length,
                             char const *name, uint32_t name_length, char *out,
                             uint32_t capacity, uint32_t &out_length) noexcept;
    uint64_t attr_set_string(char const *path, uint32_t length,
                             char const *name, uint32_t name_length,
                             char const *value, uint32_t value_length) noexcept;

    uint64_t attr_get_int32(char const *path, uint32_t length, char const *name,
                            uint32_t name_length, int32_t &out) noexcept;
    uint64_t attr_set_int32(char const *path, uint32_t length, char const *name,
                            uint32_t name_length, int32_t value) noexcept;
    uint64_t attr_get_uint32(char const *path, uint32_t length, char const *name,
                             uint32_t name_length, uint32_t &out) noexcept;
    uint64_t attr_set_uint32(char const *path, uint32_t length, char const *name,
                             uint32_t name_length, uint32_t value) noexcept;
    uint64_t attr_get_int64(char const *path, uint32_t length, char const *name,
                            uint32_t name_length, int64_t &out) noexcept;
    uint64_t attr_set_int64(char const *path, uint32_t length, char const *name,
                            uint32_t name_length, int64_t value) noexcept;
    uint64_t attr_get_uint64(char const *path, uint32_t length, char const *name,
                             uint32_t name_length, uint64_t &out) noexcept;
    uint64_t attr_set_uint64(char const *path, uint32_t length, char const *name,
                             uint32_t name_length, uint64_t value) noexcept;
    uint64_t attr_get_bool(char const *path, uint32_t length, char const *name,
                           uint32_t name_length, bool &out) noexcept;
    uint64_t attr_set_bool(char const *path, uint32_t length, char const *name,
                           uint32_t name_length, bool value) noexcept;
    uint64_t attr_get_double(char const *path, uint32_t length,
                             char const *name, uint32_t name_length,
                             double &out) noexcept;
    uint64_t attr_set_double(char const *path, uint32_t length,
                             char const *name, uint32_t name_length,
                             double value) noexcept;

    /** Raw bytes, whatever their length. */
    uint64_t attr_get_raw(char const *path, uint32_t length, char const *name,
                          uint32_t name_length, uint8_t *out, uint32_t capacity,
                          uint32_t &out_length) noexcept;
    uint64_t attr_set_raw(char const *path, uint32_t length, char const *name,
                          uint32_t name_length, uint8_t const *value,
                          uint32_t value_length) noexcept;

private:
    aegir::ipc::Consumer port_;
    uint64_t reply_[aegir::ipc::kMaxWords];
};

}  // namespace aegir::vfs

#endif  // AEGIR_VFS_H
