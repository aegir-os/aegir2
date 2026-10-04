# The vendored libjpeg-turbo, built per target by scripts/build_libjpeg.sh
# (through scripts/run_target.py's runtime bootstrap) and installed under the
# target's build directory. Included by the top-level CMakeLists when
# AEGIR_HOSTED_CXX is ON.
#
# The jpeg.datatype class decodes JPEG through it (specs/datatypes.md). The
# archive and headers are a matched pair from one build, compiled against the
# hosted runtime's full musl. Only the libjpeg API library is built -- no
# TurboJPEG -- so a consumer links only `jpeg`.

set(AEGIR_LIBJPEG_INSTALL_DIR "${CMAKE_BINARY_DIR}/libjpeg-install")

if(NOT EXISTS "${AEGIR_LIBJPEG_INSTALL_DIR}/lib/libjpeg.a")
  message(
    FATAL_ERROR
      "libjpeg-turbo not found at ${AEGIR_LIBJPEG_INSTALL_DIR}/lib/libjpeg.a. "
      "The runtime bootstrap (scripts/run_target.py) builds it before configure; "
      "run 'make build TARGET=<target>' rather than invoking cmake directly."
  )
endif()

add_library(jpeg STATIC IMPORTED GLOBAL)
set_target_properties(
  jpeg PROPERTIES IMPORTED_LOCATION "${AEGIR_LIBJPEG_INSTALL_DIR}/lib/libjpeg.a"
)
target_include_directories(jpeg SYSTEM INTERFACE "${AEGIR_LIBJPEG_INSTALL_DIR}/include")
