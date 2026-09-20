#!/bin/bash
#
# Build libc++ (with libc++abi and libunwind) for one Aegir target.
#
# The vendored LLVM runtimes build is driven against the target's already-built
# full musl: musl's headers are the C library libc++ compiles against, and its
# archive is what the final link uses. This is the tier-1 configuration
# (specs/cxx.md): exceptions and RTTI are *off*, matching the user code policy,
# so libc++'s headers and its archive agree on the `_LIBCPP_ODR_SIGNATURE` that
# embeds the exceptions choice (libcxx/include/__config). Threads stay *on* so
# <thread>/<mutex> compile and link against musl's pthread; actually starting a
# thread needs a working clone, which is a later milestone. Localization and
# std::filesystem are off until the arcs that need them.
#
# Usage: scripts/build_libcxx.sh [TARGET]     (default: aegir)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARGET="${1:-aegir}"
OUT_DIR="${ROOT_DIR}/out/${TARGET}"
BUILD_DIR="${OUT_DIR}/cxx-build"
INSTALL_DIR="${OUT_DIR}/cxx-install"
MUSL_INSTALL="${OUT_DIR}/musl-install"

LLVM_PROJECT="${ROOT_DIR}/projects/llvm-project"

if [[ -f "${INSTALL_DIR}/lib/libc++.a" ]]; then
    echo "libc++ already built for ${TARGET}: ${INSTALL_DIR}"
    exit 0
fi

if [[ ! -d "${LLVM_PROJECT}/runtimes" ]]; then
    echo "ERROR: ${LLVM_PROJECT} is not fetched; run 'make deps'" >&2
    exit 1
fi
if [[ ! -d "${MUSL_INSTALL}/include" ]]; then
    echo "ERROR: full musl not built for ${TARGET}; run scripts/build_musl.sh ${TARGET} first" >&2
    exit 1
fi

rm -rf "${BUILD_DIR}" "${INSTALL_DIR}"
mkdir -p "${BUILD_DIR}" "${INSTALL_DIR}"

# The pinned toolchain, through the shims that carry its own runtime libraries
# (scripts/env.sh).
source "${ROOT_DIR}/scripts/env.sh"

CC="$(command -v riscv64-unknown-elf-gcc)"
CXX="$(command -v riscv64-unknown-elf-g++)"
AR="$(command -v riscv64-unknown-elf-ar)"
RANLIB="$(command -v riscv64-unknown-elf-ranlib)"

# The pinned ABI (specs/build.md): hard-float rv64imafdc/lp64d. musl's headers
# are where libc++'s C dependencies come from, and -D_GNU_SOURCE is what makes
# them declare the POSIX surface libc++ uses (nanosleep, syscall, ...): the
# strict -std=c++17 hides it otherwise.
#
# _LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS forces the library traits for
# add_pointer/remove_pointer instead of the compiler builtins. GCC 14 reports
# __has_builtin(__remove_pointer) but rejects the builtin in the signature
# libc++ uses it in (__filesystem/path.h), so the builtin path does not compile.
CFLAGS="-march=rv64imafdc_zicsr_zifencei -mabi=lp64d -O2 -D_GNU_SOURCE -isystem ${MUSL_INSTALL}/include"
CXXFLAGS="${CFLAGS} -D_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS"

cd "${BUILD_DIR}"
cmake "${LLVM_PROJECT}/runtimes" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_ASM_COMPILER="${CC}" \
    -DCMAKE_AR="${AR}" \
    -DCMAKE_RANLIB="${RANLIB}" \
    -DCMAKE_C_FLAGS="${CFLAGS}" \
    -DCMAKE_CXX_FLAGS="${CXXFLAGS}" \
    -DCMAKE_ASM_FLAGS="${CFLAGS}" \
    -DCMAKE_CROSSCOMPILING=ON \
    -DCMAKE_CROSSCOMPILING_EMULATOR="" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_SKIP_RPATH=ON \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=OFF \
    -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
    -DLLVM_ENABLE_THREADS=ON \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_SPHINX=OFF \
    -DLIBCXX_ENABLE_SHARED=OFF \
    -DLIBCXX_ENABLE_STATIC=ON \
    -DLIBCXX_ENABLE_EXCEPTIONS=OFF \
    -DLIBCXX_ENABLE_RTTI=OFF \
    -DLIBCXX_ENABLE_THREADS=ON \
    -DLIBCXX_HAS_PTHREAD_API=ON \
    -DLIBCXX_ENABLE_FILESYSTEM=OFF \
    -DLIBCXX_ENABLE_LOCALIZATION=OFF \
    -DLIBCXX_HAS_MUSL_LIBC=ON \
    -DLIBCXX_CXX_ABI=libcxxabi \
    -DLIBCXX_USE_COMPILER_RT=OFF \
    -DLIBCXX_TARGET_TRIPLE="riscv64-unknown-elf" \
    -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    -DLIBCXX_INCLUDE_TESTS=OFF \
    -DLIBCXXABI_ENABLE_SHARED=OFF \
    -DLIBCXXABI_ENABLE_STATIC=ON \
    -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF \
    -DLIBCXXABI_ENABLE_THREADS=ON \
    -DLIBCXXABI_USE_COMPILER_RT=OFF \
    -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
    -DLIBCXXABI_TARGET_TRIPLE="riscv64-unknown-elf" \
    -DLIBCXXABI_INCLUDE_TESTS=OFF \
    -DLIBUNWIND_ENABLE_SHARED=OFF \
    -DLIBUNWIND_ENABLE_STATIC=ON \
    -DLIBUNWIND_IS_BAREMETAL=ON \
    -DLIBUNWIND_TARGET_TRIPLE="riscv64-unknown-elf" \
    -DLIBUNWIND_INCLUDE_TESTS=OFF

cmake --build . --target install -- -j"$(nproc)"

echo "libc++ built for ${TARGET}: ${INSTALL_DIR}"
ls -la "${INSTALL_DIR}/lib/"
