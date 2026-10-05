/*
 * aegir-cc: the LLVM-facing half -- implementation.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * This translation unit includes libc++ and LLVM headers and must not include
 * any seL4 header (see probe.h): libsel4 declares `strcpy` with C++ linkage and
 * musl's <string.h>, which both pull in, declares it with C linkage.
 */

#include "probe.h"

#include <aegir/debug.h>

#include <llvm/Support/MemoryBuffer.h>

#include <memory>
#include <string>

namespace aegir::clang_probe {

int read_file(char const *path)
{
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
        llvm::MemoryBuffer::getFile(path);
    if (!buffer) {
        aegir::debug_write("  cc: FAIL llvm::MemoryBuffer::getFile refused: ");
        aegir::debug_write(buffer.getError().message().c_str());
        aegir::debug_write("\n");
        return 1;
    }
    aegir::debug_write("  cc: llvm::MemoryBuffer read ");
    aegir::debug_write_unsigned((**buffer).getBufferSize());
    aegir::debug_write(" bytes of ");
    aegir::debug_write(path);
    aegir::debug_write("\n");
    return 0;
}

}  // namespace aegir::clang_probe
