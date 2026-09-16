/*
 * Aegir's first spawn client, and M4's root task.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Small on purpose, and deliberately C++: director starts it first because it
 * proves as much as anything can while asking for nothing -- no port, no device,
 * no filesystem (specs/director.md). It is also the honest test of the spawn
 * path: if it prints its report, then the ELF was loaded, the stack frame was
 * right, the bootstrap block arrived through the auxv, and it could signal its
 * supervisor.
 *
 * What it demonstrates:
 *   - a C++ root task compiles for this target and links against sel4runtime,
 *     libsel4, aegir-runtime and musl with no C++ standard library present;
 *   - static constructors run before main. That is sel4runtime's doing
 *     (projects/sel4runtime/src/init.c walks __preinit_array/__init_array from
 *     env.c), and it is the assumption most C++ code silently depends on;
 *   - virtual dispatch and constexpr templates produce working code;
 *   - the hard-float lp64d ABI is the one actually in use;
 *   - the kernel console is reachable (seL4_DebugPutChar, which needs
 *     KernelPrinting -- enabled in settings.cmake).
 *
 * On freestanding C++: user code is compiled with -nostdinc/-nostdinc++ and
 * there is no C++ standard library, so this uses musl's C headers (`<stdint.h>`,
 * found through musl's staged include directory) rather than <cstdint>. Note
 * also that there is no operator new/delete yet -- deliberately. Aegir will get
 * an allocation story when it gets a memory story, not before.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <aegir/ipc/port.h>
#include <aegir/log.h>
#include <sel4/sel4.h>
#include <stdint.h>

namespace {

char const *word_size_and_abi()
{
#if defined(__riscv_float_abi_double) && defined(__riscv_xlen) && __riscv_xlen == 64
    return "riscv64, hard-float lp64d";
#else
    return "NOT the pinned riscv64/lp64d ABI";
#endif
}

// A static object with dynamic initialisation: if constructors do not run,
// this stays false and the report says so.
bool constructor_ran = false;

struct ConstructorMarker {
    ConstructorMarker()
    {
        constructor_ran = true;
    }
};

[[maybe_unused]] ConstructorMarker constructor_marker;

// Virtual dispatch must survive with -fno-rtti. No virtual destructor: that
// would emit a deleting destructor and thus a reference to operator delete,
// which does not exist in a freestanding C++ program (see the note above).
class Device {
public:
    virtual char const *kind() const = 0;
};

class Console final : public Device {
public:
    char const *kind() const override
    {
        return "console";
    }
};

template <typename T>
constexpr T larger(T left, T right)
{
    return left > right ? left : right;
}

/* Floating point in userland is a kernel capability, not only an ABI choice:
 * the kernel switches FP state per thread and a freshly retyped TCB has it
 * enabled -- only the idle thread opts out, because it must not leave the
 * FPU's state dirty (kernel/src/kernel/thread.c:33 is configureIdleThread).
 * Checked here, at boot, for the same reason the static constructor is: a
 * regression would show up as arithmetic that is quietly wrong, or as a trap
 * the first time a program touched a float.
 *
 * The values are exact in binary, so this tests the FPU rather than a rounding
 * mode, and the yields in the loop put the value through context switches. */
union FloatBits {
    float value;
    uint32_t bits;
};

union DoubleBits {
    double value;
    uint64_t bits;
};

bool floating_point_works()
{
    FloatBits single;
    single.value = 1.5f + 2.25f;
    FloatBits const single_expected = [] {
        FloatBits expected;
        expected.value = 3.75f;
        return expected;
    }();
    if (single.bits != single_expected.bits) {
        return false;
    }

    /* 0.125 doubled six times is 8, and each step is exactly representable. */
    double accumulated = 1.0 / 8.0;
    for (unsigned step = 0; step < 6; ++step) {
        seL4_Yield();
        accumulated = accumulated * 2.0;
    }
    DoubleBits wide;
    wide.value = accumulated;
    DoubleBits const wide_expected = [] {
        DoubleBits expected;
        expected.value = 8.0;
        return expected;
    }();
    return wide.bits == wide_expected.bits;
}

void write_unsigned(uint64_t value)
{
    char digits[20];
    int length = 0;
    do {
        digits[length++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && length < static_cast<int>(sizeof(digits)));
    while (length > 0) {
        seL4_DebugPutChar(digits[--length]);
    }
}

void write_chars(char const *text, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        seL4_DebugPutChar(text[i]);
    }
}

void write_line(char const *label, char const *value)
{
    aegir::debug_write("  ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    aegir::debug_write(value);
    aegir::debug_write("\n");
}

void write_number(char const *label, uint64_t value)
{
    aegir::debug_write("  ");
    aegir::debug_write(label);
    aegir::debug_write(": ");
    write_unsigned(value);
    aegir::debug_write("\n");
}

}  // namespace

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);

    Console console;
    Device &device = console;  // virtual dispatch through a base reference

    /* Whatever started us describes us in the bootstrap block it handed over
     * through the auxv (specs/director.md). A program started by director has
     * one; the same program run as a root task has none, which is how it knows
     * whether there is a supervisor to answer to. */
    uint32_t service_name_length = 0;
    char const *service_name = aegir::bootstrap::name(&service_name_length);
    bool supervised = service_name_length > 0;

    aegir::debug_write("\nAegir: spawn client online\n");
    if (supervised) {
        /* The block's strings are views into it, not C strings: the length is
         * the only thing that says where the name ends. */
        aegir::debug_write("  service: ");
        write_chars(service_name, service_name_length);
        aegir::debug_write("\n");
    } else {
        write_line("service", "none: started without a bootstrap block");
    }
    write_line("device kind (virtual)", device.kind());
    write_line("target", word_size_and_abi());
    write_line("static constructors", constructor_ran ? "ran" : "DID NOT RUN");
    write_number("constexpr template max(4, 5)", larger<uint64_t>(4, 5));
    write_line("floating point", floating_point_works() ? "works" : "WRONG");

    /* Ask the logger to record that we started. This is the whole point of the
     * port we were given: a call, an answer, and a badge at the other end that
     * says who called (specs/services.md). */
    aegir::ipc::Consumer const log =
        aegir::ipc::Consumer::find(aegir::log::kPortName, aegir::log::kPortNameLength);
    if (!log.valid()) {
        write_line("log.main port", "not given");
    } else {
        aegir::ipc::Reply const answer =
            log.call(aegir::log::kMethodEvent, static_cast<uint64_t>(aegir::log::Event::Starting));
        /* A refused call comes back as an error label rather than as data, so
         * this says which happened instead of printing a nonsense answer. */
        write_line("log.main call",
                   answer.error != 0 ? "refused"
                                     : (answer.word == aegir::log::kRecorded ? "recorded" : "answered"));
    }

    /* Tell the supervisor we got here. A notification signal is one word and
     * cannot be forged into saying someone else finished, and it carries this
     * service's badge, which is how the supervisor knows who is speaking
     * (specs/director.md).
     *
     * A deliberate fault used to live here, to walk the supervision path at boot.
     * It hung the boot instead: the supervisor is not receiving faults yet, so the
     * boot thread waited for a "ready" that never came. Until that is diagnosed,
     * supervision is machinery that has not been exercised -- which is worth
     * saying out loud rather than testing a path that eats the boot. */
    /* Ready first, then the send. Ordering it this way means a send that blocks
     * can never keep the boot from finishing: the marker depends on readiness, and
     * readiness is already reported by the time the send happens. */
    if (supervised) {
        write_line("supervision", "signalling ready");
        seL4_Signal(aegir::bootstrap::kSlotSupervision);
    }
    aegir::debug_write("AEGIR_CLIENT_OK\n");

    /* Nothing here dies on purpose yet. A deliberate fault was the way to walk
     * supervision at boot, and it cannot come back until a fault reaches the
     * supervisor at all: meanwhile it hangs the boot instead of reporting
     * (specs/director.md, "Where supervision stands"). */
    aegir::halt();
}
