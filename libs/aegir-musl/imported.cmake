# The vendored full musl, built per target by scripts/build_musl.sh and
# installed under the target's build directory. Included by the top-level
# CMakeLists when AEGIR_HOSTED_CXX is ON, next to the libc++ import.
#
# The archive and the headers are a matched pair from one build; the syscall
# redirection patch that makes musl usable on seL4 is applied by `make deps`
# (third_party/patches/projects/musl) before that build.

set(AEGIR_MUSL_INSTALL_DIR "${CMAKE_BINARY_DIR}/musl-install")

if(NOT EXISTS "${AEGIR_MUSL_INSTALL_DIR}/lib/libc.a")
  message(
    FATAL_ERROR
      "full musl not found at ${AEGIR_MUSL_INSTALL_DIR}/lib/libc.a. The runtime "
      "bootstrap (scripts/run_target.py) builds it before configure; run "
      "'make build TARGET=<target>' rather than invoking cmake directly."
  )
endif()

add_library(musl_full STATIC IMPORTED GLOBAL)
set_target_properties(musl_full PROPERTIES IMPORTED_LOCATION
                                           "${AEGIR_MUSL_INSTALL_DIR}/lib/libc.a")
target_include_directories(musl_full SYSTEM INTERFACE "${AEGIR_MUSL_INSTALL_DIR}/include")
