# Import the libc++ static libraries built per target by scripts/build_libcxx.sh
# (through scripts/run_target.py's runtime bootstrap) and installed under the
# target's build directory. Included by the top-level CMakeLists when
# AEGIR_HOSTED_CXX is ON.

set(AEGIR_CXX_INSTALL_DIR "${CMAKE_BINARY_DIR}/cxx-install")

foreach(library libunwind.a libc++abi.a libc++.a)
  if(NOT EXISTS "${AEGIR_CXX_INSTALL_DIR}/lib/${library}")
    message(
      FATAL_ERROR
        "${library} not found at ${AEGIR_CXX_INSTALL_DIR}/lib. The runtime "
        "bootstrap (scripts/run_target.py) builds it before configure; run "
        "'AEGIR_HOSTED_CXX=1 make build TARGET=<target>' rather than invoking "
        "cmake directly."
    )
  endif()
endforeach()

# Stack unwinding is libgcc's, not libunwind's. The riscv64 bare-metal
# toolchain ships no libgcc_eh, but its libgcc.a carries _Unwind_* and the
# frames are registered by the crtbegin.o the link already includes. libunwind
# is built (specs/cxx.md) but its baremetal configuration wants the
# linker-provided __eh_frame_* symbols this link does not have, and whichever
# archive the linker reaches first wins -- so libunwind is deliberately not
# linked, and never gets the chance.

# libc++abi - low-level C++ ABI
add_library(cxxabi STATIC IMPORTED GLOBAL)
set_target_properties(
  cxxabi
  PROPERTIES IMPORTED_LOCATION "${AEGIR_CXX_INSTALL_DIR}/lib/libc++abi.a"
             INTERFACE_INCLUDE_DIRECTORIES
               "${AEGIR_CXX_INSTALL_DIR}/include;${AEGIR_CXX_INSTALL_DIR}/include/c++/v1"
)

# libc++ - C++ standard library. Its headers are under include/c++/v1, which is
# also where cxxabi.h lands in the runtimes install.
add_library(cxx STATIC IMPORTED GLOBAL)
set_target_properties(
  cxx
  PROPERTIES IMPORTED_LOCATION "${AEGIR_CXX_INSTALL_DIR}/lib/libc++.a"
             INTERFACE_INCLUDE_DIRECTORIES
               "${AEGIR_CXX_INSTALL_DIR}/include/c++/v1;${AEGIR_CXX_INSTALL_DIR}/include"
             INTERFACE_LINK_LIBRARIES cxxabi
)
