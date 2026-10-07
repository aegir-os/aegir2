/*
 * aegir-lld: lld on Aegir -- the linker as a program (specs/clang-on-aegir.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * Phase 1's deliverable, third one reached first: lld's own driver as a program,
 * reached through the library lld already is. This file is the whole of it --
 * lld's flavours, its error handling, its writers and its ELF reader are the
 * cross-built archives aegir-llvm imports, and `lld::lldMain` is the entry
 * lld's own tool main calls (projects/llvm-project/lld/tools/lld/lld.cpp:89-92).
 * So what runs here is lld: the same drivers, the same diagnostics, the same
 * exit code, with no reimplementation in between.
 *
 * Aegir's CMake links it the way it links every hosted program -- the runtime's
 * crt and libraries, seL4's groups, musl, libc++ and the unwind tables -- and
 * the runtime stands the process up before main, so lld carries no knowledge of
 * Aegir and no call an ordinary program would not make.
 *
 * The exit status is lld's own: 0 when it linked, its error code otherwise.
 */

#include "lld/Common/Driver.h"

#include <llvm/Support/raw_ostream.h>

#include <cstddef>

// The flavours lld's driver table names. Each LLD_HAS_DRIVER declares that
// flavour's link entry (lld/Common/Driver.h:51-57), which is what LLD_ALL_DRIVERS
// below then references -- the same five declarations lld's own tool main makes
// (projects/llvm-project/lld/tools/lld/lld.cpp:69-73).
LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(mingw)
LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(wasm)

int main(int argc, char **argv)
{
    llvm::ArrayRef<const char *> const args(argv, static_cast<std::size_t>(argc));
    lld::DriverDef drivers[] = LLD_ALL_DRIVERS;
    lld::Result const result = lld::lldMain(args, llvm::outs(), llvm::errs(), drivers);
    return result.retCode;
}
