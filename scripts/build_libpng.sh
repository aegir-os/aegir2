#!/bin/bash
#
# Build the vendored libpng for one Aegir target.
#
# The png.datatype class decodes PNG through it (specs/datatypes.md). The
# source is libpng's official release tarball, fetched and sha256-verified by
# `make deps` (manifests/sources.toml; libpng publishes no signature).
#
# libpng reads the compressed stream through zlib, so this runs after
# scripts/build_zlib.sh and points at its install. The build is out of tree and
# static: Aegir has no dynamic loader, and the vendored trees stay clean. The
# archive and headers are one matched pair, compiled against the hosted
# runtime's full musl. Hardware optimizations are off (this is RISC-V, and the
# ARM/Intel SIMD paths are not ours); the tools and tests are off too.
#
# Usage: scripts/build_libpng.sh [TARGET]     (default: aegir)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARGET="${1:-aegir}"
# The runtime is shared across targets: every Aegir target is the same ABI
# (riscv64/lp64d), so musl, libc++ and the rest are built once in out/runtime
# and read by every target. TARGET names the build for the message only.
OUT_DIR="${ROOT_DIR}/out/runtime"
BUILD_DIR="${OUT_DIR}/libpng-build"
INSTALL_DIR="${OUT_DIR}/libpng-install"
MUSL_INSTALL="${OUT_DIR}/musl-install"
ZLIB_INSTALL="${OUT_DIR}/zlib-install"

LIBPNG_SOURCE="${ROOT_DIR}/projects/libpng"

if [[ -f "${INSTALL_DIR}/lib/liblibpng16_static.a" ]]; then
    echo "libpng already built for ${TARGET}: ${INSTALL_DIR}"
    exit 0
fi

if [[ ! -f "${LIBPNG_SOURCE}/CMakeLists.txt" ]]; then
    echo "ERROR: ${LIBPNG_SOURCE} is not fetched; run 'make deps'" >&2
    exit 1
fi
if [[ ! -d "${MUSL_INSTALL}/include" ]]; then
    echo "ERROR: full musl not built for ${TARGET}; run scripts/build_musl.sh ${TARGET} first" >&2
    exit 1
fi
if [[ ! -f "${ZLIB_INSTALL}/lib/libz.a" ]]; then
    echo "ERROR: zlib not built for ${TARGET}; run scripts/build_zlib.sh ${TARGET} first" >&2
    exit 1
fi

rm -rf "${BUILD_DIR}" "${INSTALL_DIR}"
mkdir -p "${BUILD_DIR}" "${INSTALL_DIR}"

# The pinned clang toolchain and LLVM binutils (scripts/env.sh).
source "${ROOT_DIR}/scripts/env.sh"

CC="$(command -v clang)"
AR="$(command -v llvm-ar)"
RANLIB="$(command -v llvm-ranlib)"

# The pinned ABI (specs/build.md): hard-float rv64imafdc/lp64d. libpng is C and
# needs no C++ surface; -isystem pulls musl's headers in behind its own so the
# compile sees a real <stdlib.h>/<string.h>. zlib's install is on the include
# path because <png.h> includes <zlib.h>.
CFLAGS="--target=riscv64-unknown-elf -march=rv64imafdc_zicsr_zifencei -mabi=lp64d -O2 -isystem ${MUSL_INSTALL}/include -isystem ${ZLIB_INSTALL}/include"

cmake \
    -G Ninja \
    -S "${LIBPNG_SOURCE}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_C_COMPILER_TARGET="riscv64-unknown-elf" \
    -DCMAKE_AR="${AR}" \
    -DCMAKE_RANLIB="${RANLIB}" \
    -DCMAKE_C_FLAGS="${CFLAGS}" \
    -DCMAKE_SYSTEM_NAME=Generic \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_SKIP_RPATH=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DPNG_SHARED=OFF \
    -DPNG_STATIC=ON \
    -DPNG_TESTS=OFF \
    -DPNG_TOOLS=OFF \
    -DPNG_HARDWARE_OPTIMIZATIONS=OFF \
    -DZLIB_INCLUDE_DIR="${ZLIB_INSTALL}/include" \
    -DZLIB_LIBRARY="${ZLIB_INSTALL}/lib/libz.a"

cmake --build "${BUILD_DIR}" --target install -- -j"$(nproc)"

echo "libpng built for ${TARGET}: ${INSTALL_DIR}"
ls -la "${INSTALL_DIR}/lib/"
