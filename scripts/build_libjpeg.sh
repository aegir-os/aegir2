#!/bin/bash
#
# Build the vendored libjpeg-turbo for one Aegir target.
#
# The jpeg.datatype class decodes JPEG through it (specs/datatypes.md). The
# source is libjpeg-turbo's official release tarball, fetched and verified by
# `make deps` (manifests/sources.toml): the sha256, the project's detached
# signature, and its key committed at manifests/libjpeg-turbo-signing-key.asc.
#
# The build is out of tree and static: Aegir has no dynamic loader, and the
# vendored tree stays clean. Only the libjpeg API library is built -- TurboJPEG
# is off, and with it the tools (cjpeg/djpeg) -- so the IJG License is the one
# we take (THIRD-PARTY.md). Hardware optimizations are off (this is RISC-V, and
# the ARM/Intel SIMD paths are not ours); the tests are off too.
#
# Usage: scripts/build_libjpeg.sh [TARGET]     (default: aegir)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARGET="${1:-aegir}"
OUT_DIR="${ROOT_DIR}/out/${TARGET}"
BUILD_DIR="${OUT_DIR}/libjpeg-build"
INSTALL_DIR="${OUT_DIR}/libjpeg-install"
MUSL_INSTALL="${OUT_DIR}/musl-install"

LIBJPEG_SOURCE="${ROOT_DIR}/projects/libjpeg-turbo"

if [[ -f "${INSTALL_DIR}/lib/libjpeg.a" ]]; then
    echo "libjpeg-turbo already built for ${TARGET}: ${INSTALL_DIR}"
    exit 0
fi

if [[ ! -f "${LIBJPEG_SOURCE}/CMakeLists.txt" ]]; then
    echo "ERROR: ${LIBJPEG_SOURCE} is not fetched; run 'make deps'" >&2
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
AR="$(command -v riscv64-unknown-elf-ar)"
RANLIB="$(command -v riscv64-unknown-elf-ranlib)"

# The pinned ABI (specs/build.md): hard-float rv64imafdc/lp64d. libjpeg-turbo is
# C and needs no C++ surface; -isystem pulls musl's headers in behind its own so
# the compile sees a real <stdlib.h>/<string.h>.
CFLAGS="-march=rv64imafdc_zicsr_zifencei -mabi=lp64d -O2 -isystem ${MUSL_INSTALL}/include"

cmake \
    -G Ninja \
    -S "${LIBJPEG_SOURCE}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_AR="${AR}" \
    -DCMAKE_RANLIB="${RANLIB}" \
    -DCMAKE_C_FLAGS="${CFLAGS}" \
    -DCMAKE_SYSTEM_NAME=Generic \
    -DCMAKE_SYSTEM_PROCESSOR=riscv64 \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_SKIP_RPATH=ON \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DENABLE_SHARED=OFF \
    -DENABLE_STATIC=ON \
    -DWITH_SIMD=OFF \
    -DWITH_TURBOJPEG=OFF \
    -DWITH_TOOLS=OFF \
    -DWITH_TESTS=OFF \
    -DWITH_FUZZ=OFF

cmake --build "${BUILD_DIR}" --target install -- -j"$(nproc)"

echo "libjpeg-turbo built for ${TARGET}: ${INSTALL_DIR}"
ls -la "${INSTALL_DIR}/lib/"
