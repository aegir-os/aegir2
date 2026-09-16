/*
 * Aegir's initrd -- implementation. See include/aegir/spawn/initrd.h.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/spawn/initrd.h>

/* util_libs' reader. Its header has no extern "C" guard, so from C++ it has to
 * be included inside one or the link asks for mangled names the library does not
 * define (specs/userland.md). */
extern "C" {
#include <cpio/cpio.h>
}

namespace aegir::spawn {

namespace {

bool same(char const *left, unsigned left_length, char const *right, unsigned right_length) noexcept
{
    if (left_length != right_length) {
        return false;
    }
    for (unsigned i = 0; i < left_length; ++i) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

/* Entry names live in the newc header's name field, whose stored length includes
 * a terminating NUL, so the length is recoverable by scanning -- bounded by the
 * archive's longest name. */
unsigned name_length(char const *name, unsigned limit) noexcept
{
    unsigned length = 0;
    while (length < limit && name[length] != '\0') {
        ++length;
    }
    return length;
}

}  // namespace

Initrd::Initrd(void const *archive, uint64_t size) noexcept
    : archive_(archive), size_(size), entries_(0), max_name_(0), valid_(false)
{
    if (archive == nullptr || size == 0) {
        return;
    }
    struct cpio_info info {};
    if (cpio_info(archive, static_cast<unsigned long>(size), &info) != 0) {
        return;
    }
    entries_ = info.file_count;
    max_name_ = info.max_path_sz;
    valid_ = true;
}

char const *Initrd::name(unsigned index, unsigned *length) const noexcept
{
    if (!valid_ || index >= entries_) {
        return nullptr;
    }
    char const *entry_name = nullptr;
    unsigned long entry_size = 0;
    if (cpio_get_entry(archive_, static_cast<unsigned long>(size_), static_cast<int>(index),
                       &entry_name, &entry_size) == nullptr) {
        return nullptr;
    }
    if (length != nullptr) {
        *length = name_length(entry_name, max_name_);
    }
    return entry_name;
}

bool Initrd::has_duplicate_names() const noexcept
{
    for (unsigned i = 0; i < entries_; ++i) {
        unsigned left_length = 0;
        char const *left = name(i, &left_length);
        if (left == nullptr) {
            continue;
        }
        for (unsigned j = 0; j < i; ++j) {
            unsigned right_length = 0;
            char const *right = name(j, &right_length);
            if (right != nullptr && same(left, left_length, right, right_length)) {
                return true;
            }
        }
    }
    return false;
}

void const *Initrd::find(char const *name, unsigned length, uint64_t *size) const noexcept
{
    if (!valid_) {
        return nullptr;
    }
    for (unsigned i = 0; i < entries_; ++i) {
        unsigned entry_length = 0;
        char const *entry_name = this->name(i, &entry_length);
        if (entry_name == nullptr || !same(entry_name, entry_length, name, length)) {
            continue;
        }
        /* cpio_get_entry *returns* the file's data and fills the out-parameter
         * with its name -- the two are not the same thing, and swapping them
         * hands the caller the name as if it were the contents. */
        char const *ignored_name = nullptr;
        unsigned long entry_size = 0;
        void const *entry_data =
            cpio_get_entry(archive_, static_cast<unsigned long>(size_), static_cast<int>(i),
                           &ignored_name, &entry_size);
        if (entry_data == nullptr) {
            return nullptr;
        }
        if (size != nullptr) {
            *size = entry_size;
        }
        return entry_data;
    }
    return nullptr;
}

}  // namespace aegir::spawn
