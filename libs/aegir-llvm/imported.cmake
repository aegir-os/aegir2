# Import the cross-built LLVM libraries and their headers, for the on-device
# compiler (specs/clang-on-aegir.md). Included by the top-level CMakeLists when
# AEGIR_HOSTED_CXX is ON.
#
# The whole archive set is imported as one interface target. The archives have
# circular dependencies, and seL4's user-mode link rule wraps <LINK_LIBRARIES> in
# `--start-group ... --end-group`, so listing them all is enough -- the group
# rescans, and the linker pulls only the members a symbol needs.

set(AEGIR_LLVM_INSTALL_DIR "${CMAKE_BINARY_DIR}/llvm-install")
set(AEGIR_LLVM_BUILD_DIR "${CMAKE_BINARY_DIR}/llvm-build")
set(AEGIR_LLVM_SOURCE_DIR "${CMAKE_SOURCE_DIR}/projects/llvm-project")

foreach(required libLLVMSupport.a libclangFrontend.a libclangCodeGen.a liblldCommon.a)
  if(NOT EXISTS "${AEGIR_LLVM_INSTALL_DIR}/lib/${required}")
    message(
      FATAL_ERROR
        "${required} not found at ${AEGIR_LLVM_INSTALL_DIR}/lib. The LLVM build "
        "(scripts/build_llvm.sh) runs before configure; run 'make build' rather "
        "than invoking cmake directly."
    )
  endif()
endforeach()

file(GLOB AEGIR_LLVM_ARCHIVES "${AEGIR_LLVM_INSTALL_DIR}/lib/*.a")

add_library(aegir-llvm INTERFACE)
set_target_properties(aegir-llvm PROPERTIES INTERFACE_LINK_LIBRARIES
                                            "${AEGIR_LLVM_ARCHIVES}")

# SYSTEM: LLVM's and clang's headers are upstream's, not held to the hosted
# policy's -Wall -Wextra -Werror (specs/build.md). The generated headers (the
# `include` dirs under the build tree) hold clang's version and option tables.
target_include_directories(
  aegir-llvm SYSTEM
  INTERFACE "${AEGIR_LLVM_SOURCE_DIR}/llvm/include"
            "${AEGIR_LLVM_BUILD_DIR}/include"
            "${AEGIR_LLVM_SOURCE_DIR}/clang/include"
            "${AEGIR_LLVM_BUILD_DIR}/tools/clang/include")
