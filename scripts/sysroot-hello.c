/*
 * The program the on-device acceptance builds (specs/development.md,
 * specs/clang-on-aegir.md's Phase 3).
 *
 * A *hosted* Aegir program, which is what makes it a fair test: main is a plain
 * main, and the runtime's crt stands the process up before it -- so what the
 * device compiles here is exactly the shape every program in this tree has.
 *
 * It declares the one libc call it makes instead of including <unistd.h>, because
 * the sysroot's Include is the tree's shape and not yet its content: the first
 * compile is freestanding, and this file has to prove that a compile works before
 * it proves that a header does.
 *
 * What it says is the acceptance's marker: the run reads it off the console, so a
 * program the device compiled, linked and spawned is what printed it.
 */

extern long write(int fd, char const *bytes, unsigned long count);

int main(void)
{
    return write(1, "AEGIR_HELLO_OK\n", 15) == 15 ? 0 : 1;
}
