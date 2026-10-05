/*
 * aegir-net-smoke: the placeholder when AEGIR_HOSTED_CXX is OFF.
 *
 * There is no hosted runtime and so no musl socket calls, so a freestanding
 * stub keeps the manifest validation satisfied (director refuses to boot when
 * a declared binary is absent from the initrd).
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);
    aegir::debug_write("\nnet-smoke: no hosted runtime in this build\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}
