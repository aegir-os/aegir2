/*
 * aegir-partmgr: the partition manager.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The device manager starts this service once block drivers answer, and hands
 * it their ports. Its job is the filesystem-agnostic step between a block
 * device and a filesystem: read the partition table (GPT first), name the
 * partitions (BD0Part0, BD0Part1, ...), and start the filesystem service each
 * partition's type calls for, with a range grant rather than the whole device
 * (specs/services.md).
 *
 * This is the skeleton: it says who it is, reports ready, and waits. The GPT
 * walk and the filesystem launches land on top of it.
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

    aegir::debug_write("      partition manager: ready, with nothing to read yet\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
