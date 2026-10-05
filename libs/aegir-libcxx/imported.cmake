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

# Stack unwinding is libunwind's now, not libgcc's: the clang migration
# replaced libgcc with compiler-rt, and libc++abi is built with
# LIBCXXABI_USE_LLVM_UNWINDER=ON (specs/build.md). libunwind and the
# compiler-rt builtins are imported below and linked.

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

# libunwind - the unwinder libc++abi's personality and _Unwind_* calls resolve
# against now that libgcc_eh is gone.
add_library(unwind STATIC IMPORTED GLOBAL)
set_target_properties(
  unwind PROPERTIES IMPORTED_LOCATION "${AEGIR_CXX_INSTALL_DIR}/lib/libunwind.a"
)

# compiler-rt's builtins are not imported here: scripts/build_compiler_rt.sh
# publishes them as `libgcc.a` (with `crtbegin.o`/`crtend.o`) in clang's runtime
# directory, which is where seL4's user-mode link rule's `-lgcc` resolves them
# (projects/musllibc/Findmusllibc.cmake). libunwind is imported because
# `-lgcc_eh` is not found and the personality's `_Unwind_*` calls need it.
