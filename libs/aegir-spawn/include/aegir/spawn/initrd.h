/*
 * Aegir's initrd: the flat archive the boot files live in.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * There are no paths here and that is deliberate (specs/services.md): the archive
 * is built by basename and it will mount as `Initrd:` as a filesystem with no
 * subdirectories. Until the VFS exists, this is the only way director finds a
 * service's binary -- and listing it is how it checks that the manifest and the
 * image agree.
 *
 * Names are compared as ranges rather than as C strings, because that is what
 * the manifest holds: a view into the manifest text, which is not NUL
 * terminated.
 */

#ifndef AEGIR_SPAWN_INITRD_H
#define AEGIR_SPAWN_INITRD_H

#include <stdint.h>

namespace aegir::spawn {

class Initrd {
public:
    Initrd(void const *archive, uint64_t size) noexcept;

    /** False when the archive cannot be read at all. */
    bool valid() const noexcept { return valid_; }
    unsigned entries() const noexcept { return entries_; }

    /** Two entries with the same name would shadow silently -- a lookup returns
     *  the first match -- so this is a boot failure, not a surprise. */
    bool has_duplicate_names() const noexcept;

    /** The `index`th entry's name, or nullptr. Names are not NUL terminated, so
     *  the length comes back with them. */
    char const *name(unsigned index, unsigned *length) const noexcept;

    /** Look one up by name. nullptr when the archive has no such entry. */
    void const *find(char const *name, unsigned length, uint64_t *size) const noexcept;

private:
    void const *archive_;
    uint64_t size_;
    unsigned entries_;
    unsigned max_name_;
    bool valid_;
};

}  // namespace aegir::spawn

#endif  // AEGIR_SPAWN_INITRD_H
