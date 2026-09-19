#!/bin/bash
# Build complete musl v1.2.6 with full features for Aegir
# This replaces the minimal musllibc build

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/musl"
INSTALL_DIR="${ROOT_DIR}/build/musl-install"

# Check if already built and up-to-date
if [[ -f "${INSTALL_DIR}/lib/libc.a" ]]; then
    echo "musl already built and up-to-date"
    exit 0
fi

mkdir -p "${BUILD_DIR}"
mkdir -p "${INSTALL_DIR}"

# Source directory
MUSL_SOURCE="${ROOT_DIR}/projects/musl"

# Toolchain - source the environment
source "${ROOT_DIR}/scripts/env.sh"

TOOLCHAIN="${ROOT_DIR}/third_party/toolchain/riscv64-unknown-elf-gcc-14.2.0+19"
CC="${TOOLCHAIN}/shims/riscv64-unknown-elf-gcc"
CXX="${TOOLCHAIN}/shims/riscv64-unknown-elf-g++"
AR="${TOOLCHAIN}/shims/riscv64-unknown-elf-ar"
RANLIB="${TOOLCHAIN}/shims/riscv64-unknown-elf-ranlib"

# Build musl with full features
echo "Building complete musl v1.2.6 with full features..."
cd "${BUILD_DIR}"

# Configure musl with all features enabled
# Key options:
# --enable-locale       : Full locale support (locale_t, uselocale, etc.)
# --enable-iconv        : Iconv support for character encoding conversion
# --enable-threads      : Thread support (pthread)
# --enable-gcc-wrapper  : GCC wrapper for easier compilation
# --enable-static       : Build static library (required for Aegir)
# --disable-shared      : No shared libraries (Aegir uses static only)
# --target=riscv64      : Target architecture

cd "${MUSL_SOURCE}"

# Configure musl
./configure \
    --prefix="${INSTALL_DIR}" \
    --syslibdir="${INSTALL_DIR}/lib" \
    --target=riscv64 \
    --enable-locale \
    --enable-iconv \
    --enable-threads \
    --enable-gcc-wrapper \
    --enable-static \
    --disable-shared \
    --with-malloc=oldmalloc \
    CC="${CC}" \
    CFLAGS="-march=rv64imafdc_zicsr_zifencei -mabi=lp64d" \
    CROSS_COMPILE="${TOOLCHAIN}/bin/riscv64-unknown-elf-"

# Build and install
make -j$(nproc)
make install

# Also install the gcc wrapper for easier compilation
echo "musl built and installed to ${INSTALL_DIR}"
ls -la "${INSTALL_DIR}/lib/"