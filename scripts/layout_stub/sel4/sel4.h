/*
 * A host stub of the seL4 surface the trinket headers name (specs/trinket/layout.md).
 *
 * The layout conformance needs the toolkit's real Widget, Container and
 * GroupLayout, and widget.cc's damage path names Window, whose header includes
 * <aegir/console.h> and so <sel4/sel4.h>. The layout math never calls the
 * kernel, so this shadows that header with only the names the console, port and
 * input headers parse. It is never part of a target build (the real header is).
 */
#ifndef AEGIR_HOST_LAYOUT_SEL4_STUB_H
#define AEGIR_HOST_LAYOUT_SEL4_STUB_H

#include <stdint.h>

typedef uint64_t seL4_Word;
typedef seL4_Word seL4_CPtr;

/* The one aggregate a signature in application.h names. Its layout is never
 * read by the conformance. */
struct seL4_MessageInfo {
    seL4_Word words[1];
};
typedef seL4_MessageInfo seL4_MessageInfo_t;

enum : unsigned {
    seL4_MsgMaxLength = 120,
    seL4_PageBits = 12,
};

static inline int seL4_TCB_BindNotification(seL4_CPtr tcb, seL4_CPtr notification)
{
    static_cast<void>(tcb);
    static_cast<void>(notification);
    return 0;
}

#endif  /* AEGIR_HOST_LAYOUT_SEL4_STUB_H */
