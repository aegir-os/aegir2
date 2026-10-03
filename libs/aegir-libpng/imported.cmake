# The vendored libpng, built per target by scripts/build_libpng.sh (through
# scripts/run_target.py's runtime bootstrap) and installed under the target's
# build directory. Included by the top-level CMakeLists when AEGIR_HOSTED_CXX is
# ON, after the zlib import it links against.
#
# The png.datatype class decodes PNG through it (specs/datatypes.md). The
# archive and headers are a matched pair from one build, compiled against the
# hosted runtime's full musl. It links zlib in turn, so a consumer links only
# `png` and the compressed stream comes along.

set(AEGIR_LIBPNG_INSTALL_DIR "${CMAKE_BINARY_DIR}/libpng-install")

if(NOT EXISTS "${AEGIR_LIBPNG_INSTALL_DIR}/lib/liblibpng16_static.a")
  message(
    FATAL_ERROR
      "libpng not found at ${AEGIR_LIBPNG_INSTALL_DIR}/lib/liblibpng16_static.a. "
      "The runtime bootstrap (scripts/run_target.py) builds it before configure; "
      "run 'make build TARGET=<target>' rather than invoking cmake directly."
  )
endif()

add_library(png STATIC IMPORTED GLOBAL)
set_target_properties(
  png PROPERTIES IMPORTED_LOCATION
                 "${AEGIR_LIBPNG_INSTALL_DIR}/lib/liblibpng16_static.a"
)
target_include_directories(png SYSTEM INTERFACE "${AEGIR_LIBPNG_INSTALL_DIR}/include")
target_link_libraries(png INTERFACE zlib)
