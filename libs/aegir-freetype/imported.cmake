# The vendored FreeType, built per target by scripts/build_freetype.sh (through
# scripts/run_target.py's runtime bootstrap) and installed under the target's
# build directory. Included by the top-level CMakeLists when AEGIR_HOSTED_CXX is
# ON, next to the musl and libc++ imports. The font service links it
# (specs/fonts.md); nothing else does.
#
# The archive and the headers are a matched pair from one build, compiled
# against the hosted runtime's full musl, with FreeType's optional dependencies
# off (the build script's comment says why).

set(AEGIR_FREETYPE_INSTALL_DIR "${CMAKE_SOURCE_DIR}/out/runtime/freetype-install")

if(NOT EXISTS "${AEGIR_FREETYPE_INSTALL_DIR}/lib/libfreetype.a")
  message(
    FATAL_ERROR
      "FreeType not found at ${AEGIR_FREETYPE_INSTALL_DIR}/lib/libfreetype.a. "
      "The runtime bootstrap (scripts/run_target.py) builds it before configure; "
      "run 'make build TARGET=<target>' rather than invoking cmake directly."
  )
endif()

# The target name a consumer links, next to cxx/cxxabi and musl_full.
add_library(freetype STATIC IMPORTED GLOBAL)
set_target_properties(
  freetype PROPERTIES IMPORTED_LOCATION
                      "${AEGIR_FREETYPE_INSTALL_DIR}/lib/libfreetype.a"
)
# <ft2build.h> is under include/freetype2; the module headers it pulls in are
# relative to it.
target_include_directories(freetype SYSTEM INTERFACE
                           "${AEGIR_FREETYPE_INSTALL_DIR}/include/freetype2"
)
