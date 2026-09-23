/*
 * A bundle of filesystem helper images, with the registry that names them.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The partition manager starts a filesystem service per partition, and the
 * filesystem service's image travels as bytes because that is the one part of
 * spawning a capability cannot carry (specs/services.md). With more than one
 * filesystem there is more than one image, so the device manager hands them
 * all over in one blob -- the bundle -- together with the registry text that
 * says which partition type each one serves. The partition manager finds the
 * image it needs by the name the registry gave.
 *
 * The format is a header, a table of named entries, and the images. It is
 * little-endian and self-contained; nothing here knows what an image is.
 */

#ifndef AEGIR_FSBUNDLE_H
#define AEGIR_FSBUNDLE_H

#include <stdint.h>

namespace aegir::fsbundle {

constexpr uint32_t kMagic = 0x31425346u; /* "FSB1" */
constexpr uint32_t kNameMax = 32;
constexpr uint32_t kAlignment = 8;

struct Entry {
    char name[kNameMax]; /* NUL-padded; the comparison is by length */
    uint32_t offset;     /* from the bundle's first byte */
    uint32_t size;
};

struct Header {
    uint32_t magic;
    uint32_t count;
    /* Entry entries[count] follow, then the images. */
};

/** The bytes a bundle of `count` images with these sizes occupies. Returns 0
 *  when the size would overflow a uint32_t. */
uint32_t measure(uint32_t count, uint64_t const *sizes) noexcept;

/** Lay the bundle out at `out`. `names[i]` is not NUL-terminated; `lengths[i]`
 *  is its length, at most kNameMax. `images[i]`/`sizes[i]` are the bytes.
 *  Returns the bytes used, or 0 when a name is too long, an image is null, or
 *  the capacity is short. */
uint32_t build(void *out, uint32_t capacity, uint32_t count,
               char const *const *names, uint32_t const *lengths,
               void const *const *images, uint64_t const *sizes) noexcept;

/** One entry's bytes, by name, or nullptr when the bundle has no such entry.
 *  `size`, when given, receives the entry's size. */
void const *find(void const *blob, uint32_t bytes, char const *name,
                 uint32_t name_length, uint32_t *size) noexcept;

/** One row of the filesystem registry, as parsed from its text. The pointers
 *  are views into that text. */
struct Row {
    char const *type;
    uint32_t type_length; /* the GPT type GUID, canonical text */
    char const *kind;     /* the instance-name prefix: fat.BD0Part0 */
    uint32_t kind_length;
    char const *binary;   /* the initrd entry the service starts from */
    uint32_t binary_length;
};

/** True when `row`'s type text names the same GUID as a GPT partition entry's
 *  sixteen bytes. The text is canonical; the entry is GPT's mixed-endian
 *  layout, so the first three fields swap (gpt.cc writes it that way). False
 *  on a malformed text. */
bool type_matches(Row const &row, uint8_t const gpt_type[16]) noexcept;

}  // namespace aegir::fsbundle

#endif  // AEGIR_FSBUNDLE_H
