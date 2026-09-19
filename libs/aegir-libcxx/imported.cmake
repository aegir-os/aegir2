# Import libc++ static libraries built by LLVM runtimes build
# This is included by the top-level CMakeLists.txt before building Trinket

# Path to the installed runtimes
set(LIBCXX_INSTALL_DIR "${CMAKE_SOURCE_DIR}/build/libcxx-install")

# Verify the libraries exist
if(NOT EXISTS "${LIBCXX_INSTALL_DIR}/lib/libunwind.a")
  message(FATAL_ERROR "libunwind.a not found at ${LIBCXX_INSTALL_DIR}/lib/libunwind.a. Run 'scripts/build_libcxx.sh' first.")
endif()
if(NOT EXISTS "${LIBCXX_INSTALL_DIR}/lib/libc++abi.a")
  message(FATAL_ERROR "libc++abi.a not found at ${LIBCXX_INSTALL_DIR}/lib/libc++abi.a. Run 'scripts/build_libcxx.sh' first.")
endif()
if(NOT EXISTS "${LIBCXX_INSTALL_DIR}/lib/libc++.a")
  message(FATAL_ERROR "libc++.a not found at ${LIBCXX_INSTALL_DIR}/lib/libc++.a. Run 'scripts/build_libcxx.sh' first.")
endif()

# libunwind - stack unwinding
add_library(unwind STATIC IMPORTED GLOBAL)
set_target_properties(unwind PROPERTIES
  IMPORTED_LOCATION "${LIBCXX_INSTALL_DIR}/lib/libunwind.a"
  INTERFACE_INCLUDE_DIRECTORIES "${LIBCXX_INSTALL_DIR}/include"
)

# libc++abi - low-level C++ ABI
add_library(cxxabi STATIC IMPORTED GLOBAL)
set_target_properties(cxxabi PROPERTIES
  IMPORTED_LOCATION "${LIBCXX_INSTALL_DIR}/lib/libc++abi.a"
  INTERFACE_INCLUDE_DIRECTORIES "${LIBCXX_INSTALL_DIR}/include"
  INTERFACE_LINK_LIBRARIES unwind
)

# libc++ - C++ standard library
add_library(cxx STATIC IMPORTED GLOBAL)
set_target_properties(cxx PROPERTIES
  IMPORTED_LOCATION "${LIBCXX_INSTALL_DIR}/lib/libc++.a"
  INTERFACE_INCLUDE_DIRECTORIES "${LIBCXX_INSTALL_DIR}/include"
  INTERFACE_LINK_LIBRARIES cxxabi
)

# Export for parent scope
set(CXX_LIBRARIES cxx cxxabi unwind PARENT_SCOPE)
set(CXX_INCLUDE_DIRS "${LIBCXX_INSTALL_DIR}/include" PARENT_SCOPE)