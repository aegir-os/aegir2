# The vendored zlib, built per target by scripts/build_zlib.sh (through
# scripts/run_target.py's runtime bootstrap) and installed under the target's
# build directory. Included by the top-level CMakeLists when AEGIR_HOSTED_CXX is
# ON, next to the musl, libc++ and FreeType imports.
#
# libpng reads PNG's compressed stream through this (specs/datatypes.md's PNG
# class); nothing links it directly. The archive and headers are a matched pair
# from one build, compiled against the hosted runtime's full musl.

set(AEGIR_ZLIB_INSTALL_DIR "${CMAKE_SOURCE_DIR}/out/runtime/zlib-install")

if(NOT EXISTS "${AEGIR_ZLIB_INSTALL_DIR}/lib/libz.a")
  message(
    FATAL_ERROR
      "zlib not found at ${AEGIR_ZLIB_INSTALL_DIR}/lib/libz.a. "
      "The runtime bootstrap (scripts/run_target.py) builds it before configure; "
      "run 'make build TARGET=<target>' rather than invoking cmake directly."
  )
endif()

add_library(zlib STATIC IMPORTED GLOBAL)
set_target_properties(zlib PROPERTIES IMPORTED_LOCATION
                                      "${AEGIR_ZLIB_INSTALL_DIR}/lib/libz.a")
target_include_directories(zlib SYSTEM INTERFACE "${AEGIR_ZLIB_INSTALL_DIR}/include")
