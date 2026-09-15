/*
 * Aegir's root task.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Small on purpose, and deliberately C++: its job is to prove the C++ path end
 * to end on the pinned target and to fail loudly if any part of it regresses.
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

#include <aegir/debug.h>
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

    aegir::debug_write("\nAegir: root task online\n");
    write_line("device kind (virtual)", device.kind());
    write_line("target", word_size_and_abi());
    write_line("static constructors", constructor_ran ? "ran" : "DID NOT RUN");
    write_number("constexpr template max(4, 5)", larger<uint64_t>(4, 5));
    aegir::debug_write("AEGIR_BOOT_OK\n");

    // A root task has nothing to return to.
    aegir::halt();
}
