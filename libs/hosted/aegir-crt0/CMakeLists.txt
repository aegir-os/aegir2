#
# aegir-crt0: the runtime stands a hosted program up (specs/cxx.md's hosted
# runtime, specs/posix.md's acceptance).
#
# It is an OBJECT library, not a static one, and that is the point: a linker
# searches a static archive only for symbols something already references, so a
# stand-up constructor inside one is never pulled into a program that
# references nothing from the runtime -- which is exactly the program this
# exists for (aegir-runtime's debug.cc records the same failure for its own
# bridge). A linked object library's objects are on the link line, so the
# constructor is in the program and runs.
#
# It links aegir-command, which owns the stand-up itself, and aegir-runtime,
# whose halt and debug console the failure path speaks through.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-crt0 CXX)

add_library(aegir-crt0 OBJECT src/stand_up.cc)

target_link_libraries(aegir-crt0 PUBLIC
    aegir-cxx-policy-hosted
    aegir-command
    aegir-runtime
)
