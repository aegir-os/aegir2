/*
 * aegir-fs-fat: the FAT filesystem service.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * One instance per FAT partition, started by the partition manager with the
 * block device's port and a range grant (offset and length) rather than the
 * whole device (specs/services.md). FAT16/32/ExFAT are the interchange
 * filesystems -- how Aegir exchanges data with the rest of the world, not its
 * own filesystem -- and read-only to begin with.
 *
 * This is the skeleton: it says who it is, reports ready, and waits. The BPB
 * read and the root directory listing land on top of it.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (log.valid()) {
        (void)log.call(aegir::log::kMethodEvent,
                       static_cast<uint64_t>(aegir::log::Event::Starting));
    }

    aegir::debug_write("      fs.fat: ready, attached to nothing yet\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
