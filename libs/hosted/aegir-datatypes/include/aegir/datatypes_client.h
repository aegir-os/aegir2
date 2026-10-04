/*
 * The datatypes client: open a file through a class service (specs/datatypes.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A class is a user resource library: whoever opens a file starts the class
 * under their own badge through the session launcher (specs/launch.md's serve
 * kind), serving a port this client makes and keeps the other half of. The
 * client then reads the frame a page at a time. The port and the transfer page
 * are made from the caller's own allocator and window, the ServerFont shape.
 */

#ifndef AEGIR_DATATYPES_CLIENT_H
#define AEGIR_DATATYPES_CLIENT_H

#include <aegir/datatype/decoded.h>
#include <aegir/datatypes.h>
#include <aegir/ipc/port.h>
#include <aegir/mem/allocator.h>
#include <aegir/mem/vspace.h>

#include <string>
#include <string_view>

namespace aegir::datatypes {

/* An open class object: the class port, the file it decodes, and the layout the
 * class stated. Live until `dispose_object`, or until it goes out of scope. */
class Object {
public:
    Object() noexcept = default;
    ~Object();
    Object(Object &&other) noexcept;
    Object &operator=(Object &&other) noexcept;
    Object(Object const &) = delete;
    Object &operator=(Object const &) = delete;

    bool valid() const noexcept { return class_.valid(); }
    Info info() const noexcept { return info_; }

    /* Pull the whole frame -- pixels then palette, a page at a time -- into
     * `into`, which is sized from `info()`. False when the class refused or a
     * page did not arrive. */
    bool read(Decoded &into);
    bool dispose_object();

private:
    friend Object new_object(aegir::mem::Allocator &, aegir::mem::Scratch &,
                             std::string_view, std::string_view);
    void reset() noexcept;

    aegir::ipc::Consumer class_;
    /* The broker, when one started the class, and the class's badge: `reset`
     * closes through the broker (or the launcher, when the client started the
     * class itself), so the class is released when the object is done with,
     * rather than held for the session. */
    aegir::ipc::Consumer manager_;
    uint64_t class_badge_ = 0;
    aegir::mem::Allocator *allocator_ = nullptr;
    aegir::mem::Scratch *scratch_ = nullptr;
    std::string path_;
    Info info_{};
    /* The transfer page: `frame_` is mapped so the client reads the class's
     * bytes; `send_` is an unmapped copy handed over, because a mapped frame
     * cannot be mapped by the class too (aegir-trinket/server_font.cc). */
    seL4_CPtr frame_ = 0;
    uint8_t *page_ = nullptr;
    seL4_CPtr send_ = 0;
};

/* Open a file with a named class: the program `DataTypes:<class>` is started
 * under the caller's badge, serving the port this makes. `class_name` is the
 * class's file name (`ilbm.datatype`), found on the DataTypes: assign. */
Object new_object(aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch,
                  std::string_view class_name, std::string_view path);

/* Open a file, choosing the class from its extension -- the first cut; a
 * content-sniffing broker is the generalization (specs/datatypes.md). */
Object new_object(aegir::mem::Allocator &allocator, aegir::mem::Scratch &scratch,
                  std::string_view path);

} // namespace aegir::datatypes

#endif // AEGIR_DATATYPES_CLIENT_H
