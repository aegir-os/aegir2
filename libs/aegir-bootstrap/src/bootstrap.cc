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
             char const *account, uint32_t account_length, PortEntry const *ports,
             uint32_t port_count, uint64_t devices_address, uint32_t devices_bytes,
             uint64_t device_address, uint32_t device_bytes, uint64_t device_physical) noexcept
{
    if (storage == nullptr || name == nullptr || account == nullptr) {
        return nullptr;
    }
    if (port_count > 0 && ports == nullptr) {
        return nullptr;
    }

    /* Six fixed entries -- size, name, account, page bits, devices, device -- and
     * one per port,
     * because the ports a process is given are part of who it is. Growing the
     * block means bumping the version rather than gambling on a layout, and
     * `entry_count` is what makes that safe for readers that know less. */
    uint32_t const entries = 6 + port_count;
    uint64_t const header_size = sizeof(Block) + static_cast<uint64_t>(entries) * sizeof(Entry);
    uint64_t data_size = static_cast<uint64_t>(name_length) + account_length;
    for (uint32_t i = 0; i < port_count; ++i) {
        data_size += ports[i].name_length;
    }
    if (header_size + data_size > storage_size) {
        return nullptr;
    }

    auto *block = static_cast<Block *>(storage);
    block->magic = kMagic;
    block->version = kVersion;
    block->entry_count = entries;
    block->reserved = 0;

    auto *data = reinterpret_cast<char *>(storage) + header_size;
    uint64_t room = storage_size - header_size;
    uint32_t const name_copied = copy(data, name, name_length, static_cast<uint32_t>(room));
    if (name_copied != name_length) {
        return nullptr;
    }
    room -= name_length;
    uint32_t const account_copied = copy(data + name_length, account, account_length,
                                         static_cast<uint32_t>(room));
    if (account_copied != account_length) {
        return nullptr;
    }
    room -= account_length;

    uint32_t const name_offset = static_cast<uint32_t>(header_size);
    uint32_t const account_offset = static_cast<uint32_t>(header_size + name_length);
    block->entries[0] = Entry{EntryKind::Size, 0, header_size + data_size, 0, 0};
    block->entries[1] = Entry{EntryKind::Name, name_length, 0, name_offset, 0};
    block->entries[2] = Entry{EntryKind::Account, account_length, 0, account_offset, 0};
    block->entries[3] = Entry{EntryKind::PageBits, 0, seL4_PageBits, 0, 0};
    block->entries[4] =
        Entry{EntryKind::Devices, devices_address == 0 ? 0 : devices_bytes, devices_address, 0, 0};
    block->entries[5] = Entry{EntryKind::Device, device_address == 0 ? 0 : device_bytes,
                              device_physical,
                              static_cast<uint32_t>(device_address), 0};

    uint64_t next_offset = static_cast<uint64_t>(header_size) + name_length + account_length;
    for (uint32_t i = 0; i < port_count; ++i) {
        uint32_t const copied = copy(data + (next_offset - header_size), ports[i].name,
                                     ports[i].name_length, static_cast<uint32_t>(room));
        if (copied != ports[i].name_length) {
            return nullptr;
        }
        room -= ports[i].name_length;
        block->entries[6 + i] =
            Entry{EntryKind::Capability, ports[i].name_length, ports[i].slot,
                  static_cast<uint32_t>(next_offset), ports[i].size_bits};
        next_offset += ports[i].name_length;
    }
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

bool devices(uint64_t *address, uint32_t *length) noexcept
{
    Block const *block = find();
    if (block == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < block->entry_count; ++i) {
        if (block->entries[i].kind != EntryKind::Devices || block->entries[i].number == 0) {
            continue;
        }
        if (address != nullptr) {
            *address = block->entries[i].number;
        }
        if (length != nullptr) {
            *length = block->entries[i].length;
        }
        return true;
    }
    return false;
}

bool device(uint64_t *address, uint32_t *length, uint64_t *physical) noexcept
{
    Block const *block = find();
    if (block == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < block->entry_count; ++i) {
        if (block->entries[i].kind != EntryKind::Device || block->entries[i].number == 0) {
            continue;
        }
        if (address != nullptr) {
            *address = block->entries[i].data_offset;
        }
        if (length != nullptr) {
            *length = block->entries[i].length;
        }
        if (physical != nullptr) {
            *physical = block->entries[i].number;
        }
        return true;
    }
    return false;
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

bool capability(char const *name, uint32_t length, uint64_t *slot) noexcept
{
    Block const *block = find();
    if (block == nullptr || name == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < block->entry_count; ++i) {
        Entry const &entry = block->entries[i];
        if (entry.kind != EntryKind::Capability || entry.length != length) {
            continue;
        }
        char const *entry_name = reinterpret_cast<char const *>(block) + entry.data_offset;
        bool same = true;
        for (uint32_t j = 0; j < length; ++j) {
            if (entry_name[j] != name[j]) {
                same = false;
                break;
            }
        }
        if (same) {
            if (slot != nullptr) {
                *slot = entry.number;
            }
            return true;
        }
    }
    return false;
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
