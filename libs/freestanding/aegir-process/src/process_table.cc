/*
 * ProcessTable implementation (specs/process.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/process_table.h>

#include <aegir/ipc/badge.h>

namespace aegir::process {

namespace {

/* A row seen as the words the wire carries it in: 8-aligned, the pid first,
 * the same reading aegir/registry.h gives its own row. */
uint64_t const* row_words(Row const& row) noexcept
{
    return reinterpret_cast<uint64_t const*>(&row);
}

}  // namespace

ProcessTable::ProcessTable(Row* rows, uint32_t capacity) noexcept
    : rows_(rows), capacity_(capacity), count_(0)
{
}

uint32_t ProcessTable::count() const noexcept
{
    return count_;
}

Row const* ProcessTable::at(uint32_t index) const noexcept
{
    return index < count_ ? &rows_[index] : nullptr;
}

Row const* ProcessTable::find(uint64_t pid) const noexcept
{
    for (uint32_t i = 0; i < count_; ++i) {
        if (rows_[i].pid == pid) {
            return &rows_[i];
        }
    }
    return nullptr;
}

bool ProcessTable::may_break(uint64_t caller, uint64_t owner) noexcept
{
    if (!aegir::ipc::is_user_badge(caller)) {
        return true; /* the system class is the superuser */
    }
    if (!aegir::ipc::is_user_badge(owner)) {
        return false; /* a user cannot break a system-started process */
    }
    return aegir::ipc::user_index(caller) == aegir::ipc::user_index(owner);
}

bool ProcessTable::add(Row const& seed) noexcept
{
    if (count_ >= capacity_ || find(seed.pid) != nullptr) {
        return false;
    }
    rows_[count_] = seed;
    /* The registry's own, not the caller's: a new process runs and carries no
     * attention flag yet, and a name or path past its field is cut here rather
     * than read past later. */
    rows_[count_].flags = 0;
    rows_[count_].state = kStateRunning;
    rows_[count_].name[kNameMax - 1] = '\0';
    rows_[count_].path[kPathMax - 1] = '\0';
    ++count_;
    return true;
}

bool ProcessTable::remove(uint64_t pid) noexcept
{
    for (uint32_t i = 0; i < count_; ++i) {
        if (rows_[i].pid == pid) {
            /* The set is unordered: the last live row fills the hole. So an
             * index is not a position a caller may hold across a removal --
             * which is why a walker reads count/describe again rather than
             * keeping one. */
            rows_[i] = rows_[count_ - 1];
            --count_;
            return true;
        }
    }
    return false;
}

bool ProcessTable::set_flags(uint64_t pid, uint64_t flags, uint64_t caller) noexcept
{
    for (uint32_t i = 0; i < count_; ++i) {
        if (rows_[i].pid != pid) {
            continue;
        }
        if (!may_break(caller, rows_[i].owner)) {
            return false;
        }
        rows_[i].flags = flags;
        /* C is the abort: setting it leaves the process break-pending until
         * the delivery that clears it (specs/process.md). D, E and F are flags
         * only. */
        if ((flags & kAttnC) != 0) {
            rows_[i].state = kStateBreakPending;
        }
        return true;
    }
    return false;
}

uint32_t ProcessTable::handle(uint32_t method, uint64_t const* words, uint32_t word_count,
                              uint64_t caller, uint64_t* reply, uint32_t capacity) noexcept
{
    switch (method) {
    case kMethodCount:
        if (capacity < 1) {
            return 0;
        }
        reply[0] = count_;
        return 1;

    case kMethodDescribe: {
        if (word_count < 1 || capacity < kRowWords) {
            return 0;
        }
        Row const* row = at(static_cast<uint32_t>(words[0]));
        if (row == nullptr) {
            return 0;
        }
        uint64_t const* src = row_words(*row);
        for (uint32_t i = 0; i < kRowWords; ++i) {
            reply[i] = src[i];
        }
        return kRowWords;
    }

    case kMethodBreak:
        if (word_count < 2 || capacity < 1) {
            return 0;
        }
        reply[0] = set_flags(words[0], words[1] & kAttnAll, caller) ? kBreakSet
                                                                    : kBreakRefused;
        return 1;

    case kMethodRegister:
        if (word_count < kRowWords || capacity < 1) {
            return 0;
        }
        reply[0] = add(*reinterpret_cast<Row const*>(words)) ? kProcessAdded
                                                             : kProcessRefused;
        return 1;

    case kMethodUnregister:
        if (word_count < 1 || capacity < 1) {
            return 0;
        }
        reply[0] = remove(words[0]) ? kProcessAdded : kProcessRefused;
        return 1;

    default:
        return 0; /* an unknown method is answered by saying nothing */
    }
}

}  // namespace aegir::process
