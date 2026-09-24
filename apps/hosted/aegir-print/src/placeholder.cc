/*
 * aegir-print: the placeholder when AEGIR_HOSTED_CXX is OFF.
 *
 * There is no hosted runtime for a command to use, so a freestanding stub
 * keeps the initrd entry present. The terminal's spawn would find it is not a
 * hosted image and the command would fail loudly; the toolkit is off in that
 * build, so the terminal does not run.
 */

#include <aegir/bootstrap.h>
#include <aegir/debug.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[])
{
    static_cast<void>(argc);
    static_cast<void>(argv);
    aegir::debug_write("\naegir-print: no hosted runtime in this build\n");
    seL4_Signal(aegir::bootstrap::kSlotSupervision);
    aegir::halt();
}