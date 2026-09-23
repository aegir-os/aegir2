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

Block *write(void *storage, uint64_t storage_size, Contents const &contents) noexcept
{
    if (storage == nullptr || contents.name == nullptr || contents.account == nullptr) {
        return nullptr;
    }
    if (contents.port_count > 0 && contents.ports == nullptr) {
        return nullptr;
    }
    if (contents.device_cap_count > 0 && contents.device_caps == nullptr) {
        return nullptr;
    }

    /* Eleven fixed entries -- size, name, account, page bits, devices, device,
     * untyped, binaries, window, shared window, current directory -- then one per
     * device capability, then one per port, because what a process is given is
     * part of who it is. Growing the block means bumping the version rather than
     * gambling on a layout, and `entry_count` is what makes that safe for readers
     * that know less. */
    uint32_t const entries = 11 + contents.device_cap_count + contents.port_count;
    uint64_t const header_size = sizeof(Block) + static_cast<uint64_t>(entries) * sizeof(Entry);
    uint64_t data_size = static_cast<uint64_t>(contents.name_length) +
                         contents.account_length + contents.cwd_length;
    for (uint32_t i = 0; i < contents.port_count; ++i) {
        data_size += contents.ports[i].name_length;
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
    uint32_t const name_copied = copy(data, contents.name, contents.name_length,
                                      static_cast<uint32_t>(room));
    if (name_copied != contents.name_length) {
        return nullptr;
    }
    room -= contents.name_length;
    uint32_t const account_copied = copy(data + contents.name_length, contents.account,
                                         contents.account_length, static_cast<uint32_t>(room));
    if (account_copied != contents.account_length) {
        return nullptr;
    }
    room -= contents.account_length;
    uint32_t const cwd_copied =
        copy(data + contents.name_length + contents.account_length, contents.cwd,
             contents.cwd_length, static_cast<uint32_t>(room));
    if (cwd_copied != contents.cwd_length) {
        return nullptr;
    }
    room -= contents.cwd_length;

    uint32_t const name_offset = static_cast<uint32_t>(header_size);
    uint32_t const account_offset = static_cast<uint32_t>(header_size + contents.name_length);
    uint32_t const cwd_offset =
        static_cast<uint32_t>(header_size + contents.name_length + contents.account_length);
    block->entries[0] = Entry{EntryKind::Size, 0, header_size + data_size, 0, 0};
    block->entries[1] = Entry{EntryKind::Name, contents.name_length, 0, name_offset, 0};
    block->entries[2] = Entry{EntryKind::Account, contents.account_length, 0, account_offset, 0};
    block->entries[3] = Entry{EntryKind::PageBits, 0, seL4_PageBits, 0, 0};
    block->entries[4] = Entry{EntryKind::Devices,
                              contents.devices_address == 0 ? 0 : contents.devices_bytes,
                              contents.devices_address, 0, 0};
    block->entries[5] = Entry{EntryKind::Device,
                              contents.device_address == 0 ? 0 : contents.device_bytes,
                              contents.device_physical,
                              static_cast<uint32_t>(contents.device_address), 0};
    /* The same shape as a device: an address the spawner knows and the child cannot work out
     * for itself, plus a size. `reserved` carries the size in bits, as a `Capability` entry
     * carries it for the same region. */
    block->entries[6] = Entry{EntryKind::Untyped, 0, contents.untyped_physical,
                              static_cast<uint32_t>(contents.untyped_address),
                              contents.untyped_bits};
    block->entries[7] = Entry{EntryKind::Binaries,
                              contents.binaries_address == 0 ? 0 : contents.binaries_bytes,
                              contents.binaries_address, 0, 0};
    block->entries[8] = Entry{EntryKind::Window,
                              contents.window_base == 0 ? 0 : contents.window_bytes,
                              contents.window_base, 0, 0};
    /* The same shape as the untyped: an address the spawner mapped and the
     * child cannot work out for itself, plus the physical base a device (or a
     * virtqueue descriptor) needs. Present only when the child serves a data
     * port. */
    block->entries[9] = Entry{EntryKind::SharedWindow,
                              contents.shared_window_address == 0 ? 0
                                                                  : contents.shared_window_bytes,
                              contents.shared_window_physical,
                              static_cast<uint32_t>(contents.shared_window_address), 0};
    /* The current directory (specs/environment.md): a string entry, empty when
     * the child was given none. */
    block->entries[10] =
        Entry{EntryKind::CurrentDir, contents.cwd_length, 0, cwd_offset, 0};

    for (uint32_t i = 0; i < contents.device_cap_count; ++i) {
        block->entries[11 + i] =
            Entry{EntryKind::DeviceCapability, contents.device_caps[i].bytes,
                  contents.device_caps[i].physical, 0,
                  static_cast<uint32_t>(contents.device_caps[i].slot)};
    }

    uint64_t next_offset = static_cast<uint64_t>(header_size) + contents.name_length +
                           contents.account_length + contents.cwd_length;
    for (uint32_t i = 0; i < contents.port_count; ++i) {
        uint32_t const copied = copy(data + (next_offset - header_size),
                                     contents.ports[i].name,
                                     contents.ports[i].name_length,
                                     static_cast<uint32_t>(room));
        if (copied != contents.ports[i].name_length) {
            return nullptr;
        }
        room -= contents.ports[i].name_length;
        block->entries[11 + contents.device_cap_count + i] =
            Entry{EntryKind::Capability, contents.ports[i].name_length,
                  contents.ports[i].slot, static_cast<uint32_t>(next_offset),
                  contents.ports[i].size_bits};
        next_offset += contents.ports[i].name_length;
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

bool untyped(uint64_t *physical, uint32_t *size_bits, uint64_t *address) noexcept
{
    Block const *block = find();
    if (block == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < block->entry_count; ++i) {
        Entry const &entry = block->entries[i];
        if (entry.kind == EntryKind::Untyped && entry.number != 0) {
            if (physical != nullptr) {
                *physical = entry.number;
            }
            if (size_bits != nullptr) {
                *size_bits = entry.reserved;
            }
            if (address != nullptr) {
                *address = entry.data_offset;
            }
            return true;
        }
    }
    return false;
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

bool capability_size_bits(char const *name, uint32_t length,
                          uint32_t *size_bits) noexcept
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
            if (size_bits != nullptr) {
                *size_bits = entry.reserved;
            }
            return true;
        }
    }
    return false;
}

bool binaries(uint64_t *address, uint32_t *length) noexcept
{
    Block const *block = find();
    if (block == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < block->entry_count; ++i) {
        Entry const &entry = block->entries[i];
        if (entry.kind == EntryKind::Binaries && entry.number != 0) {
            if (address != nullptr) {
                *address = entry.number;
            }
            if (length != nullptr) {
                *length = entry.length;
            }
            return true;
        }
    }
    return false;
}

bool window(uint64_t *base, uint32_t *bytes) noexcept
{
    Block const *block = find();
    if (block == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < block->entry_count; ++i) {
        Entry const &entry = block->entries[i];
        if (entry.kind == EntryKind::Window && entry.number != 0) {
            if (base != nullptr) {
                *base = entry.number;
            }
            if (bytes != nullptr) {
                *bytes = entry.length;
            }
            return true;
        }
    }
    return false;
}

bool shared_window(uint64_t *address, uint32_t *bytes, uint64_t *physical) noexcept
{
    Block const *block = find();
    if (block == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < block->entry_count; ++i) {
        Entry const &entry = block->entries[i];
        if (entry.kind == EntryKind::SharedWindow && entry.number != 0) {
            if (address != nullptr) {
                *address = entry.data_offset;
            }
            if (bytes != nullptr) {
                *bytes = entry.length;
            }
            if (physical != nullptr) {
                *physical = entry.number;
            }
            return true;
        }
    }
    return false;
}

bool device_capability(uint32_t index, uint64_t *physical, uint32_t *bytes,
                       uint64_t *slot) noexcept
{
    Block const *block = find();
    if (block == nullptr) {
        return false;
    }
    uint32_t seen = 0;
    for (uint32_t i = 0; i < block->entry_count; ++i) {
        Entry const &entry = block->entries[i];
        if (entry.kind != EntryKind::DeviceCapability) {
            continue;
        }
        if (seen == index) {
            if (physical != nullptr) {
                *physical = entry.number;
            }
            if (bytes != nullptr) {
                *bytes = entry.length;
            }
            if (slot != nullptr) {
                *slot = entry.reserved;
            }
            return true;
        }
        ++seen;
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

char const *current_dir(uint32_t *length) noexcept
{
    uint32_t len = 0;
    char const *found = string(EntryKind::CurrentDir, &len);
    if (found == nullptr || len == 0) {
        if (length != nullptr) {
            *length = 0;
        }
        return nullptr;
    }
    if (length != nullptr) {
        *length = len;
    }
    return found;
}

}  // namespace aegir::bootstrap
