/*
 * The Aegir bootstrap block -- implementation. See include/aegir/bootstrap.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/bootstrap.h>

/* sel4runtime's auxv header is C++-safe (it is not the one with the C-only
 * asserts), which is what lets a child read the vector it was started with.
 * The accessor is declared here rather than by including sel4runtime.h, whose
 * sel4runtime/stdint.h uses _Static_assert: the C/C++ boundary recorded in
 * specs/userland.md. */
#include <sel4/sel4.h>
#include <sel4runtime/auxv.h>

extern "C" {
auxv_t const *sel4runtime_auxv(void);
}

namespace aegir::bootstrap {

namespace {

uint32_t copy(char *destination, char const *source, uint32_t length, uint32_t room) noexcept
{
    if (length > room) {
        return 0;
    }
    for (uint32_t i = 0; i < length; ++i) {
        destination[i] = source[i];
    }
    return length;
}

}  // namespace

Block *write(void *storage, uint64_t storage_size, char const *name, uint32_t name_length,
             char const *account, uint32_t account_length) noexcept
{
    if (storage == nullptr || name == nullptr || account == nullptr) {
        return nullptr;
    }
    /* A fixed four entries: size, name, account, page bits. Growing the block is
     * a version bump rather than a layout gamble, and `entry_count` is what makes
     * that safe for readers. */
    uint64_t const header_size = sizeof(Block) + 4 * sizeof(Entry);
    uint64_t const data_size = static_cast<uint64_t>(name_length) + account_length;
    if (header_size + data_size > storage_size) {
        return nullptr;
    }

    auto *block = static_cast<Block *>(storage);
    block->magic = kMagic;
    block->version = kVersion;
    block->entry_count = 4;
    block->reserved = 0;

    auto *data = reinterpret_cast<char *>(storage) + header_size;
    uint32_t name_copied = copy(data, name, name_length, static_cast<uint32_t>(storage_size - header_size));
    if (name_copied != name_length) {
        return nullptr;
    }
    uint32_t account_copied = copy(data + name_length, account, account_length,
                                   static_cast<uint32_t>(storage_size - header_size - name_length));
    if (account_copied != account_length) {
        return nullptr;
    }

    auto const name_offset = static_cast<uint32_t>(header_size);
    auto const account_offset = static_cast<uint32_t>(header_size + name_length);
    block->entries[0] = Entry{EntryKind::Size, 0, header_size + data_size, 0, 0};
    block->entries[1] = Entry{EntryKind::Name, name_length, 0, name_offset, 0};
    block->entries[2] = Entry{EntryKind::Account, account_length, 0, account_offset, 0};
    block->entries[3] = Entry{EntryKind::PageBits, 0, seL4_PageBits, 0, 0};
    return block;
}

Block const *find() noexcept
{
    auxv_t const *auxv = sel4runtime_auxv();
    if (auxv == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; auxv[i].a_type != AT_NULL; ++i) {
        if (auxv[i].a_type != kAuxvTag) {
            continue;
        }
        auto const *block = static_cast<Block const *>(auxv[i].a_un.a_ptr);
        if (block == nullptr || block->magic != kMagic || block->version != kVersion) {
            return nullptr;
        }
        return block;
    }
    return nullptr;
}

char const *string(EntryKind kind, uint32_t *length) noexcept
{
    Block const *block = find();
    if (block == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < block->entry_count; ++i) {
        Entry const &entry = block->entries[i];
        if (entry.kind != kind) {
            continue;
        }
        if (length != nullptr) {
            *length = entry.length;
        }
        /* Resolve the offset against *our* address, which is where the block is
         * in this process -- not the one it was written in. */
        return reinterpret_cast<char const *>(block) + entry.data_offset;
    }
    return nullptr;
}

char const *name(uint32_t *length) noexcept
{
    uint32_t const none = 0;
    char const *found = string(EntryKind::Name, length);
    if (found == nullptr) {
        if (length != nullptr) {
            *length = none;
        }
        return "";
    }
    return found;
}

}  // namespace aegir::bootstrap
