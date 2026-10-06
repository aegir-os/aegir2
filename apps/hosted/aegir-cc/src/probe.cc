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

#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Basic/TargetOptions.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/FrontendOptions.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/FrontendTool/Utils.h>

#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
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

    /* Register the targets before anything asks for one: without this the
     * codegen has no `riscv64` to build, and clang fails with nothing said
     * unless its diagnostics reach a stream. */
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    /* A freestanding source the first compile uses: no headers and no runtime,
     * just a function and a value. Deliberately the shape specs/clang-on-aegir
     * starts with, so no on-device sysroot is needed yet. */
    aegir::debug_write("  cc: opening the source for write\n");
    {
        std::error_code ec;
        llvm::raw_fd_ostream source(source_path, ec, llvm::sys::fs::OF_Text);
        if (ec) {
            aegir::debug_write("  cc: FAIL writing the source: ");
            aegir::debug_write(ec.message().c_str());
            aegir::debug_write("\n");
            return 1;
        }
        aegir::debug_write("  cc: source opened; writing\n");
        source << "int aegir_probe(void) { return 42; }\n";
        source.flush();
        aegir::debug_write("  cc: flushed; closing\n");
        source.close();
        /* If the stream took an error, raw_fd_ostream's destructor calls
         * report_fatal_error, which hangs before it prints anything here; the
         * error is taken and cleared, and said plainly instead. */
        if (source.has_error()) {
            aegir::debug_write("  cc: stream error: ");
            aegir::debug_write(source.error().message().c_str());
            aegir::debug_write("\n");
            source.clear_error();
        }
        aegir::debug_write("  cc: closed\n");
    }
    aegir::debug_write("  cc: source written\n");

    /* clang's diagnostics are captured into a string and said on the log: its
     * own printer goes to stderr, which a command's stream may not carry, and a
     * compile that fails with nothing said is worse than one that fails loudly.
     * The printer is an existing clang class -- subclassing one here would
     * reference its typeinfo, which clang is built without. */
    /* The clang objects are never torn down: the process exits when the compile
     * is done, and clang's destructors free a large object graph the process has
     * no further use for -- the teardown is where the first attempt stalled,
     * after the object was already emitted. */
    static DiagnosticOptions diag_opts;
    static std::string diag_text;
    static llvm::raw_string_ostream diag_stream(diag_text);
    CompilerInstance &ci = *new CompilerInstance();
    ci.createDiagnostics(*llvm::vfs::getRealFileSystem(),
                         new TextDiagnosticPrinter(diag_stream, &diag_opts), true);

    /* These are `-cc1` arguments, not driver ones: CreateFromArgs parses the
     * cc1 form, so a driver-only flag like `-nostdinc` is "unknown argument".
     * The source includes nothing, so the freestanding form needs no include
     * path at all. */
    std::vector<std::string> owned = {
        "-triple", "riscv64-unknown-elf", "-emit-obj", "-o",
        object_path, "-ffreestanding", "-x", "c", source_path,
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
    aegir::debug_write("  cc: invocation built; running clang\n");

    aegir::debug_write("  cc: triple=");
    aegir::debug_write(ci.getTargetOpts().Triple.c_str());
    aegir::debug_write(" action=");
    aegir::debug_write_unsigned(
        static_cast<uint64_t>(ci.getFrontendOpts().ProgramAction));
    aegir::debug_write(" inputs=");
    aegir::debug_write_unsigned(ci.getFrontendOpts().Inputs.size());
    aegir::debug_write("\n");

    if (!ExecuteCompilerInvocation(&ci)) {
        diag_text = diag_stream.str();
        aegir::debug_write("  cc: FAIL clang could not compile the source (errors=");
        aegir::debug_write_unsigned(ci.getDiagnostics().getNumErrors());
        aegir::debug_write(")\n  cc: clang said: ");
        aegir::debug_write(diag_text.c_str());
        aegir::debug_write("\n");
        return 1;
    }

    {
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
    }
    aegir::debug_write("  cc: object read back and released\n");
    return 0;
}

}  // namespace aegir::clang_probe
