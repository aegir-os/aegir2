/*
 * The metadata protocol: attributes and queries, over a volume port.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The volume protocol (aegir/volume.h) reads, lists, stats and writes a
 * file's bytes; this is the layer beside it for what a file *is*: its typed,
 * named attributes, and the queries over them (specs/bfs.md, specs/vfs.md).
 * It is shared by the service that answers, the client that asks and the
 * runtime that maps a POSIX xattr call onto it.
 *
 * The base volume methods answer a refusal with an empty reply. Metadata
 * needs to tell "this filesystem has no such attribute" from "this
 * filesystem has no metadata", so every metadata answer begins with a
 * **status word** from the table below. FAT answers kUnsupported to every one
 * of them -- it never pretends (specs/fat.md).
 *
 * Strings travel as words, the namespace protocol's shape (aegir/nmspace.h):
 * a length word, then the bytes. A method that asks for a path and an
 * attribute names them as two strings in sequence.
 */

#pragma once

#include <stdint.h>

namespace aegir::metadata {

/* The method numbers continue the volume protocol's, so one port answers
 * both. A method a version does not know is answered by saying nothing. */
constexpr uint32_t kMethodAttrStat = 12;   /* in: path, name; answer: status, type, size */
constexpr uint32_t kMethodAttrRead = 13;   /* in: path, name, offset, max; answer: status, count, bytes */
constexpr uint32_t kMethodAttrWrite = 14;  /* in: path, name, type, offset, count, bytes; answer: status, written */
constexpr uint32_t kMethodAttrRemove = 15; /* in: path, name; answer: status */
constexpr uint32_t kMethodAttrList = 16;   /* in: path, index; answer: status, name, type, size */

/* Queries (specs/bfs.md): an expression over attributes, an open cursor, and
 * a notification endpoint for the live form. Their numbers are fixed here so
 * the methods above do not move. */
constexpr uint32_t kMethodQueryOpen = 17;
constexpr uint32_t kMethodQueryNext = 18;
constexpr uint32_t kMethodQueryClose = 19;
constexpr uint32_t kMethodQueryOpenLive = 20;

/** The most bytes of a query string the service holds per open query. A
 *  longer one is refused. */
constexpr uint32_t kQueryTextMax = 128;

/** query open's answer after the status: the handle. */
constexpr uint32_t kQueryOpenTailWords = 2;

/** A live open carries the string, the flags and a token. The filesystem owns
 *  the endpoint it signals and returns a read-only copy as the answer's one
 *  capability (specs/bfs.md); the call itself carries none. */
constexpr uint32_t kQueryOpenLiveExtraWords = 2;

/** query next's answer after the status and the name string: the size and the
 *  kind, as list's answer has them. */
constexpr uint32_t kQueryTailWords = 2;

/** query open's flags: open a live query, whose endpoint is signalled when a
 *  change could affect it (specs/bfs.md). */
constexpr uint64_t kQueryFlagLive = 1;

/** Every metadata answer's first word. */
constexpr uint64_t kOk = 0;
constexpr uint64_t kUnsupported = 1;
constexpr uint64_t kNotFound = 2;
constexpr uint64_t kInvalidName = 3;
constexpr uint64_t kReadOnly = 4;
constexpr uint64_t kNoSpace = 5;
constexpr uint64_t kNotADirectory = 6;
constexpr uint64_t kIsADirectory = 7;

/** An attribute name is at most this many bytes, and never empty
 *  (specs/bfs.md's Attributes). */
constexpr uint32_t kAttrNameMax = 255;

/** The most attribute data one call carries, the envelope's words less the
 *  status and the count. A caller that wants more asks again at the next
 *  offset; this is the same bound a volume read answers within. */
constexpr uint32_t kAttrDataMax = 117 * 8;

/** attr stat's answer after the status: the type_code and the size. */
constexpr uint32_t kAttrStatTailWords = 2;

/** attr read's answer after the status: how many bytes follow. The bytes are
 *  packed next. */
constexpr uint32_t kAttrReadHeaderWords = 2;

/** attr list's answer after the status and the name string: the type_code and
 *  the size. */
constexpr uint32_t kAttrListTailWords = 2;

/** attr write's answer after the status: how many bytes were written. */
constexpr uint32_t kAttrWriteTailWords = 2;

/** A four-character type code, packed in writing order. A C++ multicharacter
 *  constant would say the same thing but warns under -Wmultichar, which the
 *  project does not keep. */
constexpr uint32_t type_code(char a, char b, char c, char d) noexcept
{
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
}

/** The type codes an attribute may carry, Haiku's four-character names
 *  (specs/bfs.md). */
constexpr uint32_t kTypeString = type_code('C', 'S', 'T', 'R');
constexpr uint32_t kTypeInt32 = type_code('L', 'O', 'N', 'G');
constexpr uint32_t kTypeUInt32 = type_code('U', 'L', 'N', 'G');
constexpr uint32_t kTypeInt64 = type_code('L', 'L', 'N', 'G');
constexpr uint32_t kTypeUInt64 = type_code('U', 'L', 'L', 'G');
constexpr uint32_t kTypeInt16 = type_code('S', 'H', 'R', 'T');
constexpr uint32_t kTypeUInt16 = type_code('U', 'S', 'H', 'T');
constexpr uint32_t kTypeInt8 = type_code('B', 'Y', 'T', 'E');
constexpr uint32_t kTypeUInt8 = type_code('U', 'B', 'Y', 'T');
constexpr uint32_t kTypeFloat = type_code('F', 'L', 'O', 'T');
constexpr uint32_t kTypeDouble = type_code('D', 'B', 'L', 'E');
constexpr uint32_t kTypeBool = type_code('B', 'O', 'O', 'L');
constexpr uint32_t kTypeRaw = type_code('R', 'A', 'W', 'T');
constexpr uint32_t kTypeMime = type_code('M', 'I', 'M', 'S');

/** The well-known attribute names (specs/bfs.md's namespace): a file's MIME
 *  type, the application that prefers it, its icons, and Aegir's Amiga
 *  tooltype list. */
constexpr char kNameType[] = "BEOS:TYPE";
constexpr char kNameAppSig[] = "BEOS:APP_SIG";
constexpr char kNameIcon[] = "BEOS:ICON";
constexpr char kNameMiniIcon[] = "BEOS:MINI_ICON";
constexpr char kNameTooltypes[] = "AEGIR:TOOLTYPES";

/* The value codecs: an attribute's data is bytes, and these are the type_code
 * shapes in the byte order BFS stores, little-endian. The typed access above
 * vfs::Volume uses them; a client that wants the raw bytes uses the metadata
 * methods directly. A string's bytes are its characters with no terminator. */

inline void put_u32(uint8_t *at, uint32_t value) noexcept
{
    for (uint32_t i = 0; i < 4; ++i) {
        at[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
    }
}

inline uint32_t get_u32(uint8_t const *at) noexcept
{
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
           (static_cast<uint32_t>(at[2]) << 16) |
           (static_cast<uint32_t>(at[3]) << 24);
}

inline void put_i32(uint8_t *at, int32_t value) noexcept
{
    put_u32(at, static_cast<uint32_t>(value));
}

inline int32_t get_i32(uint8_t const *at) noexcept
{
    return static_cast<int32_t>(get_u32(at));
}

inline void put_u64(uint8_t *at, uint64_t value) noexcept
{
    for (uint32_t i = 0; i < 8; ++i) {
        at[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
    }
}

inline uint64_t get_u64(uint8_t const *at) noexcept
{
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(at[i]) << (i * 8);
    }
    return value;
}

inline void put_i64(uint8_t *at, int64_t value) noexcept
{
    put_u64(at, static_cast<uint64_t>(value));
}

inline int64_t get_i64(uint8_t const *at) noexcept
{
    return static_cast<int64_t>(get_u64(at));
}

inline void put_bool(uint8_t *at, bool value) noexcept
{
    at[0] = value ? 1 : 0;
}

inline bool get_bool(uint8_t const *at) noexcept
{
    return at[0] != 0;
}

inline void put_double(uint8_t *at, double value) noexcept
{
    uint64_t bits = 0;
    __builtin_memcpy(&bits, &value, sizeof(bits));
    put_u64(at, bits);
}

inline double get_double(uint8_t const *at) noexcept
{
    uint64_t const bits = get_u64(at);
    double value = 0;
    __builtin_memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace aegir::metadata