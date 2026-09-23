/*
 * The little bit of ELF Aegir needs -- implementation. See include/aegir/spawn/elf.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Field offsets are the ELF64 ones, read explicitly rather than by casting the
 * image to a struct: the alignment rules for a packed file header are exactly
 * the kind of thing a cast gets away with until it does not.
 */

#include <aegir/spawn/elf.h>

namespace aegir::spawn {

namespace {

constexpr uint64_t kIdentSize = 16;
constexpr uint8_t kClass64 = 2;
constexpr uint8_t kDataLittleEndian = 1;
constexpr uint16_t kTypeExecutable = 2;
constexpr uint16_t kTypeDynamic = 3; /* position independent: also loadable */
constexpr uint16_t kMachineRiscV = 243;

/* ELF64 header field offsets. */
constexpr uint64_t kEntryOffset = 24;
constexpr uint64_t kProgramHeaderOffsetOffset = 32;
constexpr uint64_t kProgramHeaderEntrySizeOffset = 54;
constexpr uint64_t kProgramHeaderCountOffset = 56;
/* ELF64 program header field offsets (56 bytes each). */
constexpr uint64_t kPhType = 0;
constexpr uint64_t kPhFlags = 4;
constexpr uint64_t kPhOffset = 8;
constexpr uint64_t kPhVaddr = 16;
constexpr uint64_t kPhFilesz = 32;
constexpr uint64_t kPhMemsz = 40;
constexpr uint64_t kPhAlign = 48;
constexpr uint64_t kProgramHeaderSize = 56;

uint16_t read16(uint8_t const *at) noexcept
{
    return static_cast<uint16_t>(at[0]) | static_cast<uint16_t>(at[1]) << 8;
}

uint32_t read32(uint8_t const *at) noexcept
{
    uint32_t value = 0;
    for (int i = 3; i >= 0; --i) {
        value = (value << 8) | at[i];
    }
    return value;
}

uint64_t read64(uint8_t const *at) noexcept
{
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | at[i];
    }
    return value;
}

}  // namespace

bool Elf::parse(void const *image, uint64_t size) noexcept
{
    image_ = static_cast<uint8_t const *>(image);
    size_ = size;
    valid_ = false;
    load_end_ = 0;

    if (image_ == nullptr || size_ < kProgramHeaderEntrySizeOffset + 2 ||
        image_[0] != 0x7f || image_[1] != 'E' || image_[2] != 'L' || image_[3] != 'F') {
        return false;
    }
    if (image_[4 /* EI_CLASS */] != kClass64 || image_[5 /* EI_DATA */] != kDataLittleEndian) {
        return false;
    }
    if (read16(image_ + 16 /* e_type */) != kTypeExecutable &&
        read16(image_ + 16) != kTypeDynamic) {
        return false;
    }
    if (read16(image_ + 18 /* e_machine */) != kMachineRiscV) {
        return false;
    }

    entry_ = read64(image_ + kEntryOffset);
    program_header_offset_ = read64(image_ + kProgramHeaderOffsetOffset);
    program_header_size_ = read16(image_ + kProgramHeaderEntrySizeOffset);
    program_header_count_ = read16(image_ + kProgramHeaderCountOffset);

    if (program_header_count_ == 0 || program_header_size_ < kProgramHeaderSize) {
        return false;
    }
    if (program_header_offset_ + static_cast<uint64_t>(program_header_count_) * program_header_size_ >
        size_) {
        return false;
    }

    for (uint32_t i = 0; i < program_header_count_; ++i) {
        ProgramHeader header = program_header(i);
        if (header.type != kProgramHeaderLoad) {
            continue;
        }
        if (header.offset + header.filesz > size_) {
            return false;
        }
        uint64_t const end = header.vaddr + header.memsz;
        if (end > load_end_) {
            load_end_ = end;
        }
    }

    valid_ = true;
    return true;
}

ProgramHeader Elf::program_header(uint32_t index) const noexcept
{
    ProgramHeader header{};
    if (index >= program_header_count_) {
        return header;
    }
    uint8_t const *at = image_ + program_header_offset_ + static_cast<uint64_t>(index) * program_header_size_;
    header.type = read32(at + kPhType);
    header.flags = read32(at + kPhFlags);
    header.offset = read64(at + kPhOffset);
    header.vaddr = read64(at + kPhVaddr);
    header.filesz = read64(at + kPhFilesz);
    header.memsz = read64(at + kPhMemsz);
    header.align = read64(at + kPhAlign);
    return header;
}

bool Elf::copy_program_headers(void *destination, uint64_t room) const noexcept
{
    uint64_t const bytes = static_cast<uint64_t>(program_header_count_) * program_header_size_;
    if (!valid_ || destination == nullptr || bytes > room ||
        program_header_offset_ + bytes > size_) {
        return false;
    }
    auto *out = static_cast<uint8_t *>(destination);
    uint8_t const *from = image_ + program_header_offset_;
    for (uint64_t i = 0; i < bytes; ++i) {
        out[i] = from[i];
    }
    return true;
}

uint8_t const *Elf::segment_bytes(ProgramHeader const &header) const noexcept
{
    if (!valid_ || header.offset + header.filesz > size_) {
        return nullptr;
    }
    return image_ + header.offset;
}

}  // namespace aegir::spawn
