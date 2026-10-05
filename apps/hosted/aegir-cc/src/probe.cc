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

#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/FrontendTool/Utils.h>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <string>
#include <vector>

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

int compile(char const *source_path, char const *object_path)
{
    using namespace clang;

    /* A freestanding source the first compile uses: no headers and no runtime,
     * just a function and a value. Deliberately the shape specs/clang-on-aegir
     * starts with, so no on-device sysroot is needed yet. */
    {
        std::error_code ec;
        llvm::raw_fd_ostream source(source_path, ec, llvm::sys::fs::OF_Text);
        if (ec) {
            aegir::debug_write("  cc: FAIL writing the source: ");
            aegir::debug_write(ec.message().c_str());
            aegir::debug_write("\n");
            return 1;
        }
        source << "int aegir_probe(void) { return 42; }\n";
    }

    CompilerInstance ci;
    ci.createDiagnostics(*llvm::vfs::getRealFileSystem());

    std::vector<std::string> owned = {
        "-triple", "riscv64-unknown-elf", "-emit-obj", "-o",
        object_path, "-ffreestanding", "-nostdinc", "-x", "c",
        source_path,
    };
    std::vector<const char *> args;
    args.reserve(owned.size());
    for (std::string &argument : owned) {
        args.push_back(argument.c_str());
    }

    auto invocation = std::make_shared<CompilerInvocation>();
    llvm::ArrayRef<const char *> arg_ref(args.data(), args.size());
    CompilerInvocation::CreateFromArgs(*invocation, arg_ref, ci.getDiagnostics());
    ci.setInvocation(std::move(invocation));

    if (!ExecuteCompilerInvocation(&ci)) {
        aegir::debug_write("  cc: FAIL clang could not compile the source\n");
        return 1;
    }

    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> object =
        llvm::MemoryBuffer::getFile(object_path);
    if (!object) {
        aegir::debug_write("  cc: FAIL the object was not written: ");
        aegir::debug_write(object.getError().message().c_str());
        aegir::debug_write("\n");
        return 1;
    }
    aegir::debug_write("  cc: clang emitted an object of ");
    aegir::debug_write_unsigned((**object).getBufferSize());
    aegir::debug_write(" bytes\n");
    return 0;
}

}  // namespace aegir::clang_probe
