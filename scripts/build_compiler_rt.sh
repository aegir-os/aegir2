#!/bin/bash
#
# Build compiler-rt's builtins for one Aegir target, and publish them where
# clang finds them.
#
# The pinned LLVM release ships builtins for the host only (x86_64), not for
# riscv64. These are the freestanding helper routines the compiler calls when an
# operation has no instruction -- libgcc's role, which clang fills with
# compiler-rt (specs/build.md). They are built from the vendored llvm-project
# with the pinned clang, against no C library at all.
#
# seL4's user-mode link rule asks clang for `crtbegin.o`/`crtend.o` and links
# `-lgcc` (projects/musllibc/Findmusllibc.cmake, through
# tools/seL4/cmake-tool/helpers/environment_flags.cmake:97). With libgcc gone
# those names must resolve to compiler-rt. clang's runtime directory is what it
# searches for both `-print-file-name=<x>` and `-lgcc`, so the archive is
# published there as `libgcc.a`, with the crt objects beside it.
#
# Usage: scripts/build_compiler_rt.sh [TARGET]     (default: aegir)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARGET="${1:-aegir}"
OUT_DIR="${ROOT_DIR}/out/${TARGET}"
BUILD_DIR="${OUT_DIR}/builtins-build"
INSTALL_DIR="${OUT_DIR}/builtins-install"

LLVM_PROJECT="${ROOT_DIR}/projects/llvm-project"

# The pinned clang/llvm-ar/llvm-ranlib (scripts/env.sh).
source "${ROOT_DIR}/scripts/env.sh"

CC="$(command -v clang)"
AR="$(command -v llvm-ar)"
RANLIB="$(command -v llvm-ranlib)"

ARCHIVE="$(find "${INSTALL_DIR}" -name 'libclang_rt.builtins*.a' -print -quit 2>/dev/null)"
if [[ -z "${ARCHIVE}" ]]; then
    if [[ ! -d "${LLVM_PROJECT}/compiler-rt/lib/builtins" ]]; then
        echo "ERROR: ${LLVM_PROJECT}/compiler-rt is not fetched; run 'make deps'" >&2
        exit 1
    fi

    # The pinned ABI (specs/build.md): hard-float rv64imafdc/lp64d.
    CFLAGS="-march=rv64imafdc_zicsr_zifencei -mabi=lp64d -O2"

    rm -rf "${BUILD_DIR}" "${INSTALL_DIR}"
    mkdir -p "${BUILD_DIR}" "${INSTALL_DIR}"

    cd "${BUILD_DIR}"
    cmake "${LLVM_PROJECT}/compiler-rt/lib/builtins" \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
        -DCMAKE_C_COMPILER="${CC}" \
        -DCMAKE_C_COMPILER_TARGET="riscv64-unknown-elf" \
        -DCMAKE_ASM_COMPILER="${CC}" \
        -DCMAKE_ASM_COMPILER_TARGET="riscv64-unknown-elf" \
        -DCMAKE_AR="${AR}" \
        -DCMAKE_RANLIB="${RANLIB}" \
        -DCMAKE_C_FLAGS="${CFLAGS}" \
        -DCMAKE_ASM_FLAGS="${CFLAGS}" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCOMPILER_RT_BAREMETAL_BUILD=ON \
        -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
        -DCOMPILER_RT_INCLUDE_TESTS=OFF

    cmake --build . --target install -- -j"$(nproc)"
    ARCHIVE="${INSTALL_DIR}/lib/linux/libclang_rt.builtins-riscv64.a"
fi

# Publish under the names clang searches for. Done on every run, so a
# re-extracted toolchain (which drops these files) is repaired, and idempotent.
RUNTIME_DIR="$("${CC}" --target=riscv64-unknown-elf -print-runtime-dir)"
mkdir -p "${RUNTIME_DIR}"
cp -f "${ARCHIVE}" "${RUNTIME_DIR}/libgcc.a"
cp -f "${INSTALL_DIR}/lib/linux/clang_rt.crtbegin-riscv64.o" "${RUNTIME_DIR}/crtbegin.o"
cp -f "${INSTALL_DIR}/lib/linux/clang_rt.crtend-riscv64.o" "${RUNTIME_DIR}/crtend.o"

echo "compiler-rt builtins for ${TARGET}: ${ARCHIVE}"
echo "published: ${RUNTIME_DIR}/{libgcc.a,crtbegin.o,crtend.o}"
