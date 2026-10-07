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

#include "llvm/Support/LLVMDriver.h"

// clang's driver entry (projects/llvm-project/clang/tools/driver/driver.cpp:231).
extern int clang_main(int argc, char **argv, const llvm::ToolContext &tool_context);

int main(int argc, char **argv)
{
    llvm::ToolContext const context{argc > 0 && argv[0] != nullptr ? argv[0] : "clang",
                                    nullptr, false};
    return clang_main(argc, argv, context);
}
