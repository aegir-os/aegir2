/*
 * aegir-clang: clang on Aegir (specs/clang-on-aegir.md, Phase 1's deliverable).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * clang is one program with two roles: the driver, and cc1 when its own arguments
 * say so -- which the driver reaches either by re-invoking itself or, when the
 * argument is a cc1 one, in this very process
 * (projects/llvm-project/clang/tools/driver/driver.cpp:202-259). Both entries are
 * clang's own, compiled from clang's tool sources beside this file. What this file
 * supplies is the `main` that LLVM's add_clang_tool generates for clang in LLVM's
 * build, and the tool context that main hands over
 * (llvm/Support/LLVMDriver.h:14-23).
 *
 * The driver's Path matters here in a way it does not on a hosted system: clang
 * finds itself again through it, and on Aegir "again" means a spawn of this
 * program's name on the volume (specs/launch.md). No prepended argument -- this
 * program is clang, not a multiplexer.
 *
 * The exit status is clang's own.
 */

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LLVMDriver.h"

// clang's driver entry (projects/llvm-project/clang/tools/driver/driver.cpp:231).
extern int clang_main(int argc, char **argv, const llvm::ToolContext &tool_context);

int main(int argc, char **argv)
{
    /* Seed LLVM's cached answer for "where is this program?" before anything else asks.
     * getMainExecutable caches its first result, and an early caller inside LLVM passes
     * argv0 = nullptr -- which on Aegir (no /proc, and a dladdr that answers nothing) caches
     * the empty string for the whole process. The driver then takes that empty path for the
     * cc1 it re-executes, forks "", and exits 0 having compiled nothing at all
     * (specs/development.md's Phase 3 measurements; specs/clang-on-aegir.md's Phase 3).
     * Asking first, with the name the launcher gave this program, is what makes the cache
     * answer that name. */
    (void)llvm::sys::fs::getMainExecutable(argv[0], reinterpret_cast<void *>(&main));
    llvm::ToolContext const context{argc > 0 && argv[0] != nullptr ? argv[0] : "clang",
                                    nullptr, false};
    return clang_main(argc, argv, context);
}
