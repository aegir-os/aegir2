#!/bin/bash
#
# Cross-build LLVM, clang and lld *libraries* for one Aegir target.
#
# The on-device compiler (specs/clang-on-aegir.md) links libLLVM/libclang/liblld
# into an ordinary Aegir hosted program -- the in-process driver of Phase 3 --
# so only the static libraries are needed, not the stock clang/lld executables.
# That also sidesteps teaching LLVM's own CMake to link Aegir hosted programs:
# no executable is produced here, and the driver is linked by Aegir's CMake,
# which already knows the sel4runtime crt and the runtime's libraries.
#
# Only the RISCV backend is built, against the already-built full musl and
# libc++ (scripts/build_musl.sh, scripts/build_libcxx.sh). Host tablegen comes
# from the pinned LLVM release (the same 20.1.8 revision), so no nested host
# build is needed. The vendored tree is the pinned one; the tracked patches are
# already applied by `make deps`.
#
# Usage: scripts/build_llvm.sh [TARGET]     (default: aegir)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARGET="${1:-aegir}"
# The runtime is shared across targets: every Aegir target is the same ABI
# (riscv64/lp64d), so musl, libc++ and the rest are built once in out/runtime
# and read by every target. TARGET names the build for the message only.
OUT_DIR="${ROOT_DIR}/out/runtime"
BUILD_DIR="${OUT_DIR}/llvm-build"
INSTALL_DIR="${OUT_DIR}/llvm-install"
MUSL_INSTALL="${OUT_DIR}/musl-install"
CXX_INSTALL="${OUT_DIR}/cxx-install"

LLVM_PROJECT="${ROOT_DIR}/projects/llvm-project"

if [[ -f "${INSTALL_DIR}/lib/libclang.a" && -f "${INSTALL_DIR}/lib/liblldCommon.a" ]]; then
    echo "LLVM libraries already built for ${TARGET}: ${INSTALL_DIR}"
    exit 0
fi

if [[ ! -f "${LLVM_PROJECT}/llvm/CMakeLists.txt" ]]; then
    echo "ERROR: ${LLVM_PROJECT} is not fetched; run 'make deps'" >&2
    exit 1
fi
if [[ ! -d "${MUSL_INSTALL}/include" ]]; then
    echo "ERROR: full musl not built for ${TARGET}; run scripts/build_musl.sh ${TARGET} first" >&2
    exit 1
fi
if [[ ! -f "${CXX_INSTALL}/lib/libc++.a" ]]; then
    echo "ERROR: libc++ not built for ${TARGET}; run scripts/build_libcxx.sh ${TARGET} first" >&2
    exit 1
fi

# The pinned clang toolchain and LLVM binutils (scripts/env.sh).
source "${ROOT_DIR}/scripts/env.sh"

CC="$(command -v clang)"
CXX="$(command -v clang++)"
AR="$(command -v llvm-ar)"
RANLIB="$(command -v llvm-ranlib)"
HOST_TOOL_BIN="$(dirname "$(command -v llvm-tblgen)")"

# The build triple for Aegir's *hosted* tier is Linux-musl, not the bare-metal
# `riscv64-unknown-elf` the freestanding tier uses. LLVM's Support library picks
# its Unix implementation by macro and has no generic path (Unix/Process.inc
# `#error`s without one); Aegir's hosted C runtime is musl and its syscall ABI is
# Linux's -- the `__sysinfo` dispatcher answers Linux riscv64 numbers -- so the
# Linux paths LLVM selects are the ones Aegir implements.
BUILD_TRIPLE="riscv64-unknown-linux-musl"

# The pinned ABI (specs/build.md): hard-float rv64imafdc/lp64d. libc++'s headers
# come *before* musl's: libc++'s <string.h>/<math.h>/... wrap the C library's
# and refuse to compile if the C library's are seen first. -D_GNU_SOURCE is what
# makes musl's headers declare the POSIX surface LLVM's Support library uses.
CFLAGS="--target=${BUILD_TRIPLE} -march=rv64imafdc_zicsr_zifencei -mabi=lp64d -O2 -D_GNU_SOURCE -D_LIBCPP_AEGIR -isystem ${CXX_INSTALL}/include/c++/v1 -isystem ${CXX_INSTALL}/include -isystem ${MUSL_INSTALL}/include"

# A clean tree is the default; AEGIR_LLVM_NO_CLEAN reuses the build directory to
# iterate on configuration without recompiling every object.
if [[ -z "${AEGIR_LLVM_NO_CLEAN:-}" ]]; then
    rm -rf "${BUILD_DIR}" "${INSTALL_DIR}"
fi
mkdir -p "${BUILD_DIR}" "${INSTALL_DIR}"

cd "${BUILD_DIR}"
cmake "${LLVM_PROJECT}/llvm" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_ASM_COMPILER="${CC}" \
    -DCMAKE_C_COMPILER_TARGET="${BUILD_TRIPLE}" \
    -DCMAKE_CXX_COMPILER_TARGET="${BUILD_TRIPLE}" \
    -DCMAKE_ASM_COMPILER_TARGET="${BUILD_TRIPLE}" \
    -DCMAKE_AR="${AR}" \
    -DCMAKE_RANLIB="${RANLIB}" \
    -DCMAKE_C_FLAGS="${CFLAGS}" \
    -DCMAKE_CXX_FLAGS="${CFLAGS}" \
    -DCMAKE_ASM_FLAGS="${CFLAGS}" \
    -DCMAKE_CROSSCOMPILING=ON \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_EXE_LINKER_FLAGS="" \
    -DLLVM_ENABLE_PROJECTS="clang;lld" \
    -DLLVM_TARGETS_TO_BUILD="RISCV" \
    -DLLVM_INCLUDE_TOOLS=OFF \
    -DLLVM_INCLUDE_UTILS=OFF \
    -DLLVM_BUILD_UTILS=OFF \
    -DLLVM_BUILD_TOOLS=OFF \
    -DCLANG_BUILD_TOOLS=OFF \
    -DLLVM_NATIVE_TOOL_DIR="${HOST_TOOL_BIN}" \
    -DLLVM_TABLEGEN="${HOST_TOOL_BIN}/llvm-tblgen" \
    -DCLANG_TABLEGEN="${HOST_TOOL_BIN}/clang-tblgen" \
    -DLLVM_ENABLE_THREADS=OFF \
    -DLLVM_ENABLE_EH=OFF \
    -DLLVM_ENABLE_RTTI=OFF \
    -DLLVM_BUILD_LLVM_DYLIB=OFF \
    -DLLVM_ENABLE_PIC=OFF \
    -DLIBCLANG_BUILD_STATIC=ON \
    -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_CURL=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF \
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
    -DCLANG_ENABLE_ARCMT=OFF \
    -DCLANG_ENABLE_Z3_SOLVER=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_APPEND_VC_REV=OFF

# The `all` target includes the target-side `llvm-tblgen` executable
# (utils/TableGen is added unconditionally and add_tablegen puts it in `all`).
# It cannot be linked for Aegir -- it is a host tool, and we provide the host's
# through LLVM_TABLEGEN -- so its link is the one expected failure. `-k 0`
# builds everything else; the archives, which do not depend on that executable,
# are what the driver links.
cmake --build . -- -k 0 -j"$(nproc)" || true
for required in libLLVMSupport.a libclangFrontend.a libclangCodeGen.a liblldCommon.a liblldELF.a; do
    if [[ ! -f "lib/${required}" ]]; then
        echo "ERROR: ${required} was not built" >&2
        exit 1
    fi
done
mkdir -p "${INSTALL_DIR}/lib"
cp -f lib/*.a "${INSTALL_DIR}/lib/"

echo "LLVM libraries built for ${TARGET}: ${INSTALL_DIR}"
ls "${INSTALL_DIR}/lib/" | grep -E "libLLVM|libclang|liblld" | head
