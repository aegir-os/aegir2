# Import the cross-built LLVM libraries and their headers, for the on-device
# compiler (specs/clang-on-aegir.md). Included by the top-level CMakeLists when
# AEGIR_HOSTED_CXX is ON.
#
# Only Support (with its Demangle dependency) is imported so far: the first
# measurement reads a file through llvm::MemoryBuffer, which lives there. More
# archives join as the driver grows toward a full compile.

set(AEGIR_LLVM_INSTALL_DIR "${CMAKE_BINARY_DIR}/llvm-install")
set(AEGIR_LLVM_SOURCE_DIR "${CMAKE_SOURCE_DIR}/projects/llvm-project")

foreach(archive libLLVMSupport.a libLLVMDemangle.a)
  if(NOT EXISTS "${AEGIR_LLVM_INSTALL_DIR}/lib/${archive}")
    message(
      FATAL_ERROR
        "${archive} not found at ${AEGIR_LLVM_INSTALL_DIR}/lib. The LLVM build "
        "(scripts/build_llvm.sh) runs before configure; run 'make build' rather "
        "than invoking cmake directly."
    )
  endif()
endforeach()

# An imported target's include directories are treated as SYSTEM, so LLVM's
# headers are not held to the hosted policy's -Wall -Wextra -Werror -- they are
# upstream's, not ours (specs/build.md).
add_library(aegir-llvm-support STATIC IMPORTED GLOBAL)
set_target_properties(
  aegir-llvm-support
  PROPERTIES IMPORTED_LOCATION "${AEGIR_LLVM_INSTALL_DIR}/lib/libLLVMSupport.a"
             INTERFACE_INCLUDE_DIRECTORIES
               "${AEGIR_LLVM_SOURCE_DIR}/llvm/include;${CMAKE_BINARY_DIR}/llvm-build/include"
             INTERFACE_LINK_LIBRARIES
               "${AEGIR_LLVM_INSTALL_DIR}/lib/libLLVMDemangle.a")
