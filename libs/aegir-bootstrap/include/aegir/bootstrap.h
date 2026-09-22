/*
 * The Aegir bootstrap block and the slots a spawned process starts with.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * This is the ABI between whoever creates a process and the process itself
 * (specs/director.md). It has two halves:
 *
 *   - the CSpace slots every Aegir process is given, so a child can name itself
 *     and its supervisor without being told where to look;
 *   - one page, mapped read-only, describing who the child is and what it was
 *     granted, found through an auxv entry rather than through a convention of
 *     memory layout.
 *
 * The auxv route works because sel4runtime reads the entries it knows and
 * ignores the rest (`projects/sel4runtime/src/env.c:316-317`), and because it
 * exposes the vector it was started with (`sel4runtime_auxv`,
 * `projects/sel4runtime/include/sel4runtime.h:44`). So Aegir adds a tag instead
 * of patching the runtime.
 *
 * The block is versioned and counted: a reader that knows less than the writer
 * skips what it does not understand, which is what keeps this from being a
 * one-way door.
 */

#ifndef AEGIR_BOOTSTRAP_H
#define AEGIR_BOOTSTRAP_H

#include <stdint.h>

namespace aegir::bootstrap {

/* --- the slots every Aegir process starts with (specs/director.md) --------- */

/** Left null on purpose: a null capability must fail, not do something. */
constexpr uint64_t kSlotNull = 0;
/* The next two are seL4's own convention, not ours to choose:
 * seL4_CapInitThreadTCB is 1 and seL4_CapInitThreadCNode is 2
 * (kernel/libsel4/include/sel4/bootinfo_types.h:17-18), and the runtime and the
 * kernel's debugging paths address a thread's TCB there by name -- a child whose
 * slot 1 held something else is stopped by the kernel with "cap is not a TCB"
 * the moment it names itself (projects/sel4runtime/src/env.c:185). Our layout
 * therefore *extends* seL4's initial slots instead of rearranging them. */
/** The child's own TCB, so it can set its own priority and affinity. */
constexpr uint64_t kSlotOwnTcb = 1;
/** The child's own root CNode, so it can name capabilities it creates. */
constexpr uint64_t kSlotOwnCNode = 2;
/** The endpoint its faults are delivered to. It lives in *our* CSpace and in
 *  the child's, because seL4 requires the fault endpoint to be addressable from
 *  the thread being configured (out/aegir/libsel4/include/interfaces/sel4_client.h:876). */
constexpr uint64_t kSlotFaultEndpoint = 3;
/** The notification a child signals to say it has finished starting, which is
 *  what its supervisor waits on. One way, unforgeable, and cheap. */
constexpr uint64_t kSlotSupervision = 4;
/** Where a capability transferred over IPC lands (specs/vfs.md): one scratch
 *  slot, named with seL4_SetCapReceivePath before a call that expects a cap
 *  back, and moved out of immediately -- a second transfer onto an occupied
 *  slot fails. */
constexpr uint64_t kSlotReceiveCap = 5;
/** First slot the manifest's own declarations may use. */
constexpr uint64_t kSlotFirstDeclared = 8;

/* The size of the CSpace every Aegir process is given, in slots-bits: 2^12
 *  slots. It is part of the layout rather than a detail of the spawner, because
 *  two things depend on it: the guard a process's TCB is configured with
 *  (seL4_WordBits - kCNodeBits, so plain slot numbers resolve at full depth),
 *  and how a *service* addresses its own CSpace as a mint source -- its
 *  own-CNode cap at kSlotOwnCNode is a raw copy with guard 0 and radix
 *  kCNodeBits, so its slots resolve as plain numbers at depth kCNodeBits, not
 *  at seL4_WordBits (kernel/src/kernel/cspace.c:126-193: with no guard, the
 *  index is read MSB-first).
 *
 * It was 10 (1024) until the buddy allocator landed: a buddy split takes two
 * capabilities (the two equal halves), where the old geometric split took one,
 * so a spawning service's CSpace filled about twice as fast -- the device
 * manager hit 1001 of its 1014 free slots spawning the second GPU. Growing the
 * CSpace on demand (a two-level CSpace) is still the deferral it always was;
 * this is the size that holds until then. */
constexpr uint32_t kCNodeBits = 12;

/* --- the block ------------------------------------------------------------- */

/** Our auxv tag. Standard and seL4 tags occupy 0-72
 *  (projects/sel4runtime/include/sel4runtime/auxv.h:9-29), so Aegir's start
 *  above them. */
constexpr int kAuxvTag = 80;

constexpr uint32_t kMagic = 0x41474253; /* "AGBS" */
constexpr uint32_t kVersion = 5;

/** What a block entry describes. Unknown kinds are the reader's problem to
 *  skip, not an error. */
enum class EntryKind : uint32_t {
    /** The block's own byte size, so a reader can trust the count. */
    Size = 1,
    /** A NUL-free string: the child's service name. */
    Name = 2,
    /** The account the child is charged to. */
    Account = 3,
    /** A port the child was given: the slot in `number`, and its name at
     *  `data_offset`. The name is the identity; the slot is a layout detail. */
    Capability = 4,
    /** The page size the child's mappings use, for anything that has to agree. */
    PageBits = 5,
    /** A blob mapped into the child's address space: its address in `number` and
     *  its byte count in `length`. The device tree arrives this way -- the child
     *  reads it in place, and what it says the machine is belongs to whoever is
     *  given it, not to whoever spawned the process (specs/services.md). */
    Devices = 6,
    /** A device's registers, mapped into the child: its address in `number` and its
     *  byte count in `length`. Separate from `Devices`, which is the machine's
     *  description of itself, because a device is not a description -- a driver is
     *  given the one its service is for (specs/services.md). */
    Device = 7,
    /*  A device's registers are described by two addresses, and both are needed:
     *  `number` is the device's *physical* address -- which device it is, since
     *  identical transports are told apart only by where they are -- and
     *  `data_offset` is where the child can read them. `length` is the size. */
    /** Memory the child owns the authority for: `number` is the region's *physical*
     *  base and `reserved` its size in bits. Two things need the physical address
     *  and only the spawner knows it -- a device reads a virtqueue by physical
     *  address, and a service cannot ask the kernel where its own memory is. The
     *  capability itself travels as a `Capability` entry named the same, which is
     *  where its slot is: the slot is what the child retypes from, the physical base
     *  is what it tells the device (specs/services.md, specs/authority.md). */
    Untyped = 8,
    /** A copy of the flat initrd, mapped into the child: `number` is its address,
     *  `length` its byte count. A service that starts processes reads their images
     *  out of it -- the binaries are the one part of spawning that cannot be
     *  delegated as a capability, so they travel as bytes (specs/services.md). */
    Binaries = 9,
    /** A window of unused addresses in the child's *own* address space: `number`
     *  is its base, `length` its byte count. Only meaningful together with the
     *  child's own VSpace root (a `Capability` entry named `vspace`): a service
     *  that fills frames for the processes it starts writes through a window it
     *  can map into, and RISC-V has no narrower mapping authority than the root
     *  (specs/services.md). */
    Window = 10,
    /** A device's frame, given as a *capability* rather than a mapping: `number`
     *  is the device's physical address, `length` its byte count, and `reserved`
     *  the slot the frame capability was installed in. Nothing is mapped: the
     *  service maps the frame through its own window when it wants to read the
     *  registers, and hands it to a driver when it starts one -- which is what a
     *  device manager is *for* (specs/services.md). */
     DeviceCapability = 11,
    /** The shared window a device's port serves through (aegir/block.h): mapped
     *  into the child, so `data_offset` is where the child reads and writes it,
     *  `length` its byte count, and `number` its *physical* base -- a driver
     *  points virtqueue descriptors at it, and a device reads by physical
     *  address. The window travels with the service port because a request's
     *  data crosses there, not in the message. */
    SharedWindow = 12,
    /** The child's current directory: a VFS path (`Volume:component/path`), a
     *  string entry like `Name`. Empty or absent when the child was given none
     *  (specs/environment.md). Arguments and environment variables do not ride
     *  here: they are the startup frame's argc/argv/envp, the ABI the runtime
     *  already reads. */
    CurrentDir = 13,
};

struct Entry {
    EntryKind kind;
    uint32_t length;      /* bytes of this entry's data */
    uint64_t number;      /* what `number` means depends on `kind` */
    /* Where the data is, as a byte offset from the block's own start -- not a
     * pointer. The block is written in the writer's address space and read in
     * the child's, at a different address, so a pointer in here would be a
     * number that means nothing to whoever reads it. */
    uint32_t data_offset;
    /* Unused by the entries above. A `Capability` entry uses it for the size in bits
     * of what the capability is, when what it is has a size -- an untyped a service is
     * given to retype objects out of -- and zero otherwise. The name is history: the
     * field has always been here, and a service cannot ask the kernel how large an
     * untyped is, so the block has to say (specs/authority.md). */
    uint32_t reserved;
};

struct Block {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t reserved;
    Entry entries[];
};

/* --- writing (director) ---------------------------------------------------- */

/** A port a child is being given: its name, and the slot it was installed in
 *  (specs/services.md). The child looks a port up by name -- the name is the
 *  identity, the slot is an artifact of a layout the child did not choose. */
struct PortEntry {
    char const *name;
    uint32_t name_length;
    uint64_t slot;
    /** Zero for a port. For a capability that is memory, how big it is in bits: a
     *  service cannot ask the kernel how large an untyped is, so the block has to say
     *  (specs/authority.md). */
    uint32_t size_bits;
};

/** A device frame being handed over as a capability (EntryKind::DeviceCapability). */
struct DeviceCapEntry {
    uint64_t physical;
    uint32_t bytes;
    uint64_t slot;
};

/** Everything a block says, in one place: the parameter list had grown past what
 *  a call site could be trusted to read. Zero/empty fields mean "not given". */
struct Contents {
    char const *name;
    uint32_t name_length;
    char const *account;
    uint32_t account_length;
    /* The child's current directory (specs/environment.md), or empty for none. */
    char const *cwd;
    uint32_t cwd_length;
    PortEntry const *ports;
    uint32_t port_count;
    uint64_t devices_address;
    uint32_t devices_bytes;
    uint64_t device_address;
    uint32_t device_bytes;
    uint64_t device_physical;
    uint64_t untyped_physical;
    uint32_t untyped_bits;
    uint64_t untyped_address;
    uint64_t binaries_address;
    uint32_t binaries_bytes;
    uint64_t window_base;
    uint32_t window_bytes;
    DeviceCapEntry const *device_caps;
    uint32_t device_cap_count;
    uint64_t shared_window_address;
    uint32_t shared_window_bytes;
    uint64_t shared_window_physical;
};

/** Build a block in memory we can write: `storage` is a page that will be
 *  mapped into the child. Returns the block, or nullptr when it does not fit.
 *  `name`, `account` and every port name must outlive the call (they are copied
 *  in). */
Block *write(void *storage, uint64_t storage_size, Contents const &contents) noexcept;

/* --- reading (a spawned process) ------------------------------------------- */

/** The block this process was started with, or nullptr when there is none --
 *  which is the case for the root task, so callers have to handle it. */
Block const *find() noexcept;

/** The child's service name, or the empty string. */
char const *name(uint32_t *length) noexcept;

/** The child's current directory, or nullptr when it was given none
 *  (specs/environment.md). The pointer is into the block, valid as long as the
 *  block is. */
char const *current_dir(uint32_t *length) noexcept;

/** Look up one string entry by kind. The pointer is into the block, so it is
 *  valid for as long as the block is. */
char const *string(EntryKind kind, uint32_t *length) noexcept;

/** The slot a named port was installed in. False when this process was not given
 *  that port. */
bool capability(char const *name, uint32_t length, uint64_t *slot) noexcept;

/** A blob the process was given, and where it is. False when there is none. */
bool devices(uint64_t *address, uint32_t *length) noexcept;

/** A device's registers the process was given, and where they are. False when this
 *  process was given no device. */
bool device(uint64_t *address, uint32_t *length, uint64_t *physical) noexcept;

/** The memory region this process was given to lay objects out in: its *physical* base in
 *  `physical` and its size in bits in `size_bits`. False when it was given none. The
 *  capability is found with `capability()` under the same name; a slot is what the child
 *  retypes from, and the physical base is what it tells a device. */
bool untyped(uint64_t *physical, uint32_t *size_bits, uint64_t *address) noexcept;

/** The initrd copy this process was given, and where it is. False when there is
 *  none -- which is every process that may not start processes of its own. */
bool binaries(uint64_t *address, uint32_t *length) noexcept;

/** The window of unused addresses in this process's own address space, when its
 *  spawner said it has one. False when there is none. */
bool window(uint64_t *base, uint32_t *bytes) noexcept;

/** The `index`th device frame this process was given as a capability: its
 *  physical address, its size and the slot the capability is in. False when
 *  there is no such entry. */
bool device_capability(uint32_t index, uint64_t *physical, uint32_t *bytes,
                       uint64_t *slot) noexcept;

/** The shared window this process's service port serves through, when it has
 *  one: where it is mapped, how big it is, and its physical base. False when
 *  there is none -- which is every process that does not serve a data port. */
bool shared_window(uint64_t *address, uint32_t *bytes, uint64_t *physical) noexcept;

}  // namespace aegir::bootstrap

#endif  // AEGIR_BOOTSTRAP_H
