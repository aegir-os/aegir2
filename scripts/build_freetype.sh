#!/bin/bash
#
# Build the vendored FreeType for one Aegir target.
#
# The font service rasterizes OpenType faces through it (specs/fonts.md): one
# library serves every client, rather than every app carrying its own outline
# engine. The source is FreeType's own signed release tarball, fetched and
# signature-verified by `make deps` (manifests/sources.toml).
#
# The build is out of tree -- the vendored tree is a pinned checkout we keep
# clean -- and it is static: Aegir has no dynamic loader, so there is nothing
# for a .so to attach to. The optional dependencies are off, because Aegir
# ships none of them and a module that wanted one should fail to link here
# rather than fall back at run time: zlib, bzip2, PNG, HarfBuzz and Brotli.
# What is left is the TrueType/OpenType/CFF/BDF engine with FreeType's own
# uncompressed-PCF and LZW support. Headers and the archive are one matched
# pair, compiled against the hosted runtime's full musl.
#
# Usage: scripts/build_freetype.sh [TARGET]     (default: aegir)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARGET="${1:-aegir}"
# The runtime is shared across targets: every Aegir target is the same ABI
# (riscv64/lp64d), so musl, libc++ and the rest are built once in out/runtime
# and read by every target. TARGET names the build for the message only.
OUT_DIR="${ROOT_DIR}/out/runtime"
BUILD_DIR="${OUT_DIR}/freetype-build"
INSTALL_DIR="${OUT_DIR}/freetype-install"
MUSL_INSTALL="${OUT_DIR}/musl-install"

FREETYPE_SOURCE="${ROOT_DIR}/projects/freetype"

if [[ -f "${INSTALL_DIR}/lib/libfreetype.a" ]]; then
    echo "FreeType already built for ${TARGET}: ${INSTALL_DIR}"
    exit 0
fi

if [[ ! -f "${FREETYPE_SOURCE}/CMakeLists.txt" ]]; then
    echo "ERROR: ${FREETYPE_SOURCE} is not fetched; run 'make deps'" >&2
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

# The pinned ABI (specs/build.md): hard-float rv64imafdc/lp64d. FreeType is a C
# library and needs no C++ surface; -isystem pulls musl's headers in behind its
# own so the compile sees a real <stdlib.h>/<string.h>.
CFLAGS="--target=riscv64-unknown-elf -march=rv64imafdc_zicsr_zifencei -mabi=lp64d -O2 -isystem ${MUSL_INSTALL}/include"

cmake \
    -G Ninja \
    -S "${FREETYPE_SOURCE}" \
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
    -DFT_DISABLE_ZLIB=TRUE \
    -DFT_DISABLE_BZIP2=TRUE \
    -DFT_DISABLE_PNG=TRUE \
    -DFT_DISABLE_HARFBUZZ=TRUE \
    -DFT_DISABLE_BROTLI=TRUE \
    -DFT_ENABLE_ERROR_STRINGS=TRUE

cmake --build "${BUILD_DIR}" --target install -- -j"$(nproc)"

echo "FreeType built for ${TARGET}: ${INSTALL_DIR}"
ls -la "${INSTALL_DIR}/lib/"
