/*
 * aegir-shell: the placeholder when AEGIR_HOSTED_CXX is OFF.
 *
 * There is no hosted runtime for the shell to use, so a freestanding stub
 * keeps the initrd entry present. The terminal does not run in that build, so
 * nothing starts it.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);
    aegir::debug_write("\naegir-shell: no hosted runtime in this build\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}