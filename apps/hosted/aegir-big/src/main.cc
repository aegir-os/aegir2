/*
 * aegir-big: the scale acceptance's deliberately huge command (specs/memory.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A program's size is never bounded by the process that starts it
 * (specs/memory.md). The rule has been met by three separate walls -- a retype
 * larger than the kernel's fan-out limit, capabilities that outgrew a single
 * CNode, a segment whose frames were mapped one 4 KiB page at a time -- and
 * every one was found by a program growing past a number. This command is that
 * growth on purpose: its loaded segment is a generated blob (scripts/
 * gen_blob.py) tens of megabytes long, so bringing it up exercises the whole
 * spawn path at a size a cap would break, and it runs on the development
 * target beside the compiler so a cap that creeps back fails the acceptance by
 * name.
 *
 * It does not merely map and exit: it checks every loaded byte against the
 * pattern the generator wrote. A blob of zeros would pass even if the loader
 * never filled a frame, because a zero page and an unfilled page read the same;
 * the pattern does not, so the comparison tests the loader and not only its
 * bookkeeping.
 */

#include <aegir/command.h>
#include <aegir/debug.h>

#include <cstdlib>

extern "C" {
extern const unsigned char aegir_big_blob_start[];
extern const unsigned char aegir_big_blob_end[];
}

int main(int argc, char **argv)
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    if (!aegir::command::start("aegir-big")) {
        std::_Exit(127);
    }

    unsigned long long const count =
        static_cast<unsigned long long>(aegir_big_blob_end - aegir_big_blob_start);

    /* Every loaded byte against the pattern, positionally: a sum would miss a
     * page mapped in the wrong place, and the pattern is periodic with a period
     * that divides 4096 (scripts/gen_blob.py writes `i & 0xff` over 4096-byte
     * blocks), so `(i & 0xff)` is what byte i must be. A zero page and an
     * unfilled page read alike; this does not. */
    unsigned long long matching = 0;
    for (unsigned long long i = 0; i < count; ++i) {
        if (aegir_big_blob_start[i] == static_cast<unsigned char>(i & 0xFF)) {
            ++matching;
        }
    }

    /* The floor is what makes this a scale test and not a smoke: if the blob
     * were ever misconfigured small, the command would pass while proving
     * nothing. 16 MiB is the old single-L2-CNode ceiling (4096 pages), so the
     * command refuses to call itself OK below twice that. */
    unsigned long long const kFloorBytes = 32ull << 20;
    /* The marker goes out the debug serial, not stdout: a command's stdout
     * reaches the session's grid (fd 1 -> the terminal stream), which the
     * acceptance reads as pixels, while the serial log is where its lines are
     * text. This is why the compiler beside it prints through debug_write too. */
    if (count >= kFloorBytes && matching == count) {
        aegir::debug_write("AEGIR_BIG_OK ");
        aegir::debug_write_unsigned(count);
        aegir::debug_write(" bytes, every byte the pattern\n");
        return 0;
    }
    aegir::debug_write("AEGIR_BIG_FAIL ");
    aegir::debug_write_unsigned(count);
    aegir::debug_write(" bytes, ");
    aegir::debug_write_unsigned(matching);
    aegir::debug_write(" matched\n");
    return 1;
}
