/*
 * aegir-env-smoke: the placeholder when AEGIR_HOSTED_CXX is OFF.
 *
 * There is no hosted runtime to stand up, so a freestanding stub keeps the
 * manifest validation satisfied (director refuses to boot when a declared
 * binary is absent from the initrd). The cxx-smoke's placeholder is the shape.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);
    aegir::debug_write("\nenv-smoke: no hosted runtime in this build\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
