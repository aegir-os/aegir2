#
# aegir-clang: clang on Aegir (specs/clang-on-aegir.md, Phase 1's deliverable).
#
# clang's own tool sources, built by Aegir's CMake the way every hosted program is
# (apps/hosted/aegir-lld is the same shape, and its CMakeLists says why the link is
# ours rather than LLVM's own CMake's): the driver and the cc1 entries live in the
# *tool*, not in the libraries -- projects/llvm-project/clang/tools/driver/
# CMakeLists.txt:38-42 builds clang from driver.cpp, cc1_main.cpp, cc1as_main.cpp
# and cc1gen_reproducer_main.cpp -- so this app compiles them and supplies the main
# LLVM's add_clang_tool would have generated (src/main.cc). What runs is clang: its
# driver, its cc1, its diagnostics, its flags, its exit status.
#
# THE WARNING STANCE, deliberately and visibly: those four files are upstream's
# code, so they are compiled with upstream's warning policy (-w), exactly as the
# tree already treats LLVM's and clang's *headers* (libs/aegir-llvm/imported.cmake:
# 31-33, specs/build.md). It is policy for foreign code, not a silenced warning of
# ours: everything this tree writes -- src/main.cc, the harnesses -- is held to
# -Wall -Wextra -Werror, and so is every other app here.
#
# When AEGIR_HOSTED_CXX is OFF a freestanding placeholder keeps the build whole.
#

cmake_minimum_required(VERSION 3.16.0)

project(aegir-clang CXX)

set(AEGIR_CLANG_TOOL_DIR "${CMAKE_SOURCE_DIR}/projects/llvm-project/clang/tools/driver")

if(AEGIR_HOSTED_CXX)
  add_library(aegir-clang-tool OBJECT
    "${AEGIR_CLANG_TOOL_DIR}/driver.cpp"
    "${AEGIR_CLANG_TOOL_DIR}/cc1_main.cpp"
    "${AEGIR_CLANG_TOOL_DIR}/cc1as_main.cpp"
    "${AEGIR_CLANG_TOOL_DIR}/cc1gen_reproducer_main.cpp"
  )

  # Upstream's code, upstream's policy (see the header), and upstream's own
  # compile settings *as the libraries these sources link against were built*
  # (scripts/build_llvm.sh:84's CFLAGS, plus LLVM's own -fno-rtti -fno-exceptions):
  # -D_GNU_SOURCE and -D_LIBCPP_AEGIR are the contract musl's and libc++'s headers
  # are read under -- without them libc++'s locale_base_api.h names strtof_l and
  # friends, which musl's headers only declare with _GNU_SOURCE. The C library's
  # and libc++'s headers come from those imported targets rather than from
  # aegir-cxx-policy-hosted, whose flags (-frtti -fexceptions) belong to our code.
  target_compile_options(aegir-clang-tool PRIVATE -w -O2 -fno-rtti -fno-exceptions
                                                 -D_GNU_SOURCE -D_LIBCPP_AEGIR)
  target_link_libraries(aegir-clang-tool PRIVATE aegir-llvm cxx cxxabi unwind musl_full)

  add_executable(aegir-clang src/main.cc)

  target_compile_options(aegir-clang PRIVATE -Wall -Wextra -Werror -fno-rtti)

  # aegir-crt0 first: the stand-up constructor has to be on the link line, which
  # is why it is an object library (libs/hosted/aegir-crt0). aegir-clang-tool is
  # clang's four tool sources; aegir-llvm is LLVM, clang and lld as the
  # cross-built archives.
  target_link_libraries(
    aegir-clang PRIVATE aegir-crt0 aegir-clang-tool aegir-cxx-policy-hosted
                        aegir-cxxabi-shim aegir-heap aegir-llvm aegir-runtime
                        aegir-bootstrap aegir-mem sel4 sel4runtime
  )
else()
  add_executable(aegir-clang src/placeholder.cc)

  target_compile_options(aegir-clang PRIVATE -Wall -Wextra -Werror)

  target_link_libraries(
    aegir-clang PRIVATE aegir-cxx-policy aegir-runtime aegir-bootstrap sel4 muslc
                        sel4runtime
  )
endif()
