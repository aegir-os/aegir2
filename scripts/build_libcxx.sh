#!/bin/bash
# Build libc++ for Aegir using LLVM runtimes build system
# Fixed to avoid -rdynamic linker flag issue

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/libcxx-runtimes"
INSTALL_DIR="${ROOT_DIR}/build/libcxx-install"

# Check if already built and up-to-date
if [[ -f "${INSTALL_DIR}/lib/libc++.a" ]]; then
    # Check if any source is newer than the install
    NEWER_SOURCE=$(find "${ROOT_DIR}/projects/llvm-project" -name "*.cpp" -o -name "*.h" -o -name "*.td" -o -name "*.txt" -o -name "*.txt" -o -name "CMakeLists.txt" 2>/dev/null | head -1 | xargs -I{} stat -c "%Y" {} 2>/dev/null | sort -rn | head -1)
    INSTALL_TIME=$(stat -c "%Y" "${INSTALL_DIR}/lib/libc++.a" 2>/dev/null || echo 0)
    if [[ -n "${NEWER_SOURCE}" && "${NEWER_SOURCE}" -le "${INSTALL_TIME}" ]]; then
        echo "libc++ already built and up-to-date"
        exit 0
    fi
fi

mkdir -p "${BUILD_DIR}"
mkdir -p "${INSTALL_DIR}"

# Source directories
LLVM_PROJECT="${ROOT_DIR}/projects/llvm-project"
RUNTIMES_SOURCE="${LLVM_PROJECT}/runtimes"

# Toolchain - source the environment
source "${ROOT_DIR}/scripts/env.sh"

TOOLCHAIN="${ROOT_DIR}/third_party/toolchain/riscv64-unknown-elf-gcc-14.2.0+19"
CC="${TOOLCHAIN}/shims/riscv64-unknown-elf-gcc"
CXX="${TOOLCHAIN}/shims/riscv64-unknown-elf-g++"
AR="${TOOLCHAIN}/shims/riscv64-unknown-elf-ar"
RANLIB="${TOOLCHAIN}/shims/riscv64-unknown-elf-ranlib"

# Find musl sysroot from Aegir build (needs standard C headers for libunwind)
# Use the aegir-2g-smp2 build which has musllibc built
MUSL_STAGE="${ROOT_DIR}/out/aegir-2g-smp2/musllibc/build-temp/stage"
MUSL_INCLUDE="${MUSL_STAGE}/include"
if [[ ! -d "${MUSL_INCLUDE}" ]]; then
    echo "ERROR: musl headers not found at ${MUSL_INCLUDE}"
    echo "Build the aegir-2g-smp2 target first to generate musl headers"
    exit 1
fi

echo "Using musl headers: ${MUSL_INCLUDE}"

# Build the runtimes using LLVM's runtimes build system
echo "Building LLVM runtimes (libc++, libcxxabi, libunwind)..."
cd "${BUILD_DIR}"
cmake "${ROOT_DIR}/projects/llvm-project/runtimes" \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
  -DCMAKE_C_COMPILER="${CC}" \
  -DCMAKE_CXX_COMPILER="${CXX}" \
  -DCMAKE_ASM_COMPILER="${CC}" \
  -DCMAKE_AR="${AR}" \
  -DCMAKE_RANLIB="${RANLIB}" \
  -DCMAKE_C_FLAGS="-march=rv64imafdc_zicsr_zifencei -mabi=lp64d -isystem ${MUSL_INCLUDE}" \
  -DCMAKE_CXX_FLAGS="-march=rv64imafdc_zicsr_zifencei -mabi=lp64d -isystem ${MUSL_INCLUDE}" \
  -DCMAKE_ASM_FLAGS="-march=rv64imafdc_zicsr_zifencei -mabi=lp64d -isystem ${MUSL_INCLUDE}" \
  -DCMAKE_EXE_LINKER_FLAGS="" \
  -DCMAKE_SHARED_LINKER_FLAGS="" \
  -DCMAKE_MODULE_LINKER_FLAGS="" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_CROSSCOMPILING=ON \
  -DCMAKE_CROSSCOMPILING_EMULATOR="" \
  -DCMAKE_SKIP_RPATH=ON \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=OFF \
  -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
  -DLIBCXX_ENABLE_SHARED=OFF \
  -DLIBCXX_ENABLE_STATIC=ON \
  -DLIBCXX_ENABLE_EXCEPTIONS=ON \
  -DLIBCXX_ENABLE_RTTI=ON \
  -DLIBCXX_ENABLE_THREADS=OFF \
  -DLIBCXX_ENABLE_FILESYSTEM=ON \
  -DLIBCXX_ENABLE_LOCALIZATION=ON \
  -DLIBCXX_HAS_MUSL_LIBC=ON \
  -DLIBCXX_CXX_ABI=libcxxabi \
  -DLIBCXX_USE_COMPILER_RT=OFF \
  -DLIBCXX_TARGET_TRIPLE="riscv64-unknown-elf" \
  -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
  -DLIBCXX_INCLUDE_TESTS=OFF \
  -DLIBCXXABI_ENABLE_SHARED=OFF \
  -DLIBCXXABI_ENABLE_STATIC=ON \
  -DLIBCXXABI_ENABLE_EXCEPTIONS=ON \
  -DLIBCXXABI_ENABLE_THREADS=OFF \
  -DLIBCXXABI_USE_COMPILER_RT=OFF \
  -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
  -DLIBCXXABI_TARGET_TRIPLE="riscv64-unknown-elf" \
  -DLIBCXXABI_INCLUDE_TESTS=OFF \
  -DLIBUNWIND_ENABLE_SHARED=OFF \
  -DLIBUNWIND_ENABLE_STATIC=ON \
  -DLIBUNWIND_TARGET_TRIPLE="riscv64-unknown-elf" \
  -DLIBUNWIND_INCLUDE_TESTS=OFF \
  -DLLVM_ENABLE_THREADS=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_ENABLE_SPHINX=OFF \
  -DCMAKE_CROSSCOMPILING=ON \
  -DCMAKE_CROSSCOMPILING_EMULATOR="" \
  -DCMAKE_EXE_LINKER_FLAGS="" \
  -DCMAKE_SHARED_LINKER_FLAGS="" \
  -DCMAKE_MODULE_LINKER_FLAGS="" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_SKIP_RPATH=ON \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=OFF

# Build with make (configured for Makefile, not Ninja)
make -j$(nproc) install

echo "Libc++ runtimes built and installed to ${INSTALL_DIR}"
ls -la "${INSTALL_DIR}/lib/"