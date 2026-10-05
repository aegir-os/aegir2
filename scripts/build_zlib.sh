#!/bin/bash
#
# Build the vendored zlib for one Aegir target.
#
# libpng reads PNG's compressed stream through zlib (specs/datatypes.md's PNG
# class), so zlib is a build dependency of that class. The source is zlib's own
# signed release tarball, fetched and signature-verified by `make deps`
# (manifests/sources.toml, Mark Adler's key).
#
# The build is out of tree -- the vendored tree is a pinned checkout we keep
# clean -- and it is static: Aegir has no dynamic loader, so there is nothing
# for a .so to attach to. libpng links this archive in turn
# (scripts/build_libpng.sh). Headers and the archive are one matched pair,
# compiled against the hosted runtime's full musl.
#
# Usage: scripts/build_zlib.sh [TARGET]     (default: aegir)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARGET="${1:-aegir}"
# The runtime is shared across targets: every Aegir target is the same ABI
# (riscv64/lp64d), so musl, libc++ and the rest are built once in out/runtime
# and read by every target. TARGET names the build for the message only.
OUT_DIR="${ROOT_DIR}/out/runtime"
BUILD_DIR="${OUT_DIR}/zlib-build"
INSTALL_DIR="${OUT_DIR}/zlib-install"
MUSL_INSTALL="${OUT_DIR}/musl-install"

ZLIB_SOURCE="${ROOT_DIR}/projects/zlib"

if [[ -f "${INSTALL_DIR}/lib/libz.a" ]]; then
    echo "zlib already built for ${TARGET}: ${INSTALL_DIR}"
    exit 0
fi

if [[ ! -f "${ZLIB_SOURCE}/CMakeLists.txt" ]]; then
    echo "ERROR: ${ZLIB_SOURCE} is not fetched; run 'make deps'" >&2
    exit 1
fi
if [[ ! -d "${MUSL_INSTALL}/include" ]]; then
    echo "ERROR: full musl not built for ${TARGET}; run scripts/build_musl.sh ${TARGET} first" >&2
    exit 1
fi

rm -rf "${BUILD_DIR}" "${INSTALL_DIR}"
mkdir -p "${BUILD_DIR}" "${INSTALL_DIR}"

# The pinned clang toolchain and LLVM binutils (scripts/env.sh).
source "${ROOT_DIR}/scripts/env.sh"

CC="$(command -v clang)"
AR="$(command -v llvm-ar)"
RANLIB="$(command -v llvm-ranlib)"

# The pinned ABI (specs/build.md): hard-float rv64imafdc/lp64d. zlib is C and
# needs no C++ surface; -isystem pulls musl's headers in behind its own so the
# compile sees a real <stdlib.h>/<string.h>.
CFLAGS="--target=riscv64-unknown-elf -march=rv64imafdc_zicsr_zifencei -mabi=lp64d -O2 -isystem ${MUSL_INSTALL}/include"

cmake \
    -G Ninja \
    -S "${ZLIB_SOURCE}" \
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
    -DZLIB_BUILD_SHARED=OFF \
    -DZLIB_BUILD_STATIC=ON \
    -DZLIB_BUILD_TESTING=OFF

cmake --build "${BUILD_DIR}" --target install -- -j"$(nproc)"

echo "zlib built for ${TARGET}: ${INSTALL_DIR}"
ls -la "${INSTALL_DIR}/lib/"
