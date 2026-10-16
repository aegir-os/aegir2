/*
 * The little bit of ELF Aegir needs to start a program.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Our own types rather than an upstream header: the ELF headers we could borrow
 * come from C-only headers, and the spawner needs a handful of fields, not a
 * linker.
 *
 * Program headers are decoded on demand from the image rather than stored, so
 * there is no array whose capacity has to be guessed (project rule).
 */

#ifndef AEGIR_SPAWN_ELF_H
#define AEGIR_SPAWN_ELF_H

#include <stdint.h>

namespace aegir::spawn {

struct ProgramHeader {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

class Elf {
public:
    static constexpr uint32_t kProgramHeaderLoad = 1;
    /** The program's declared stack, from the GNU_STACK program header
     *  (`-Wl,-z,stack-size=`): its `p_memsz` is the maximum extent the program
     *  says its stack needs (specs/pager.md). Zero is the host lld's default and
     *  means the program declared nothing. */
    static constexpr uint32_t kProgramHeaderGnuStack = 0x6474e551;

    /** Parse an image out of the initrd. `valid()` says whether it worked. */
    bool parse(void const *image, uint64_t size) noexcept;

    bool valid() const noexcept { return valid_; }
    uint64_t entry() const noexcept { return entry_; }
    uint32_t program_headers() const noexcept { return program_header_count_; }
    uint32_t program_header_size() const noexcept { return program_header_size_; }
    /** Where the program headers sit in the image, for `AT_PHDR`. */
    uint64_t program_header_offset() const noexcept { return program_header_offset_; }
    ProgramHeader program_header(uint32_t index) const noexcept;
    /** The file bytes a loadable segment describes. */
    uint8_t const *segment_bytes(ProgramHeader const &header) const noexcept;
    /** Copy the program header table out, because a child is told where it is
     *  through `AT_PHDR` and that address has to be in the child's memory (the
     *  runtime reads PT_TLS from it -- projects/sel4runtime/src/env.c:322). */
    bool copy_program_headers(void *destination, uint64_t room) const noexcept;
    /** One past the highest address a loadable segment occupies: where a spawner
     *  may start placing its own regions without landing inside the image. */
    uint64_t load_end() const noexcept { return load_end_; }
    /** The maximum stack extent the program declares (`PT_GNU_STACK.p_memsz`),
     *  or zero when it declares none. `-Wl,-z,stack-size=` writes it. */
    uint64_t stack_size() const noexcept { return stack_size_; }

private:
    uint8_t const *image_;
    uint64_t size_;
    uint64_t entry_;
    uint64_t program_header_offset_;
    uint32_t program_header_count_;
    uint32_t program_header_size_;
    uint64_t load_end_;
    uint64_t stack_size_;
    bool valid_;
};

}  // namespace aegir::spawn

#endif  // AEGIR_SPAWN_ELF_H
