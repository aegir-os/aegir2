#!/bin/bash
#
# Build the vendored full musl for one Aegir target.
#
# The seL4 build has its own musllibc fork, built with locale, iconv and
# threads off and the oldmalloc backend (projects/musllibc/CMakeLists.txt).
# The hosted C++ runtime needs the real library, so this builds the vendored
# upstream musl (projects/musl) with its defaults -- mallocng, the backend
# that actually returns memory -- and with Aegir's syscall redirection patch
# applied (third_party/patches/projects/musl): musl's syscalls then reach the
# kernel through sel4_vsyscall instead of a raw ecall, which seL4 does not
# answer.
#
# The build is out of tree. musl's configure writes its Makefile into the
# current directory, and the vendored tree is a pinned checkout we keep clean.
#
# Usage: scripts/build_musl.sh [TARGET]     (default: aegir)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARGET="${1:-aegir}"
OUT_DIR="${ROOT_DIR}/out/${TARGET}"
BUILD_DIR="${OUT_DIR}/musl-build"
INSTALL_DIR="${OUT_DIR}/musl-install"

MUSL_SOURCE="${ROOT_DIR}/projects/musl"
PATCH="${ROOT_DIR}/third_party/patches/projects/musl/0001-riscv64-redirect-syscalls-to-sel4-vsyscall.patch"

if [[ -f "${INSTALL_DIR}/lib/libc.a" ]]; then
    echo "musl already built for ${TARGET}: ${INSTALL_DIR}"
    exit 0
fi

if [[ ! -d "${MUSL_SOURCE}" ]]; then
    echo "ERROR: ${MUSL_SOURCE} is not fetched; run 'make deps'" >&2
    exit 1
fi

# The syscall redirection is what makes musl usable on seL4; without it every
# syscall is a raw ecall the kernel does not understand. `make deps` applies
# it (scripts/apply_patches.py), and this refuses to build a musl that lacks
# it rather than producing a library that faults on its first syscall.
if ! git -C "${MUSL_SOURCE}" apply --reverse --check "${PATCH}" >/dev/null 2>&1; then
    echo "ERROR: the musl syscall patch is not applied; run 'make deps'" >&2
    exit 1
fi

rm -rf "${BUILD_DIR}" "${INSTALL_DIR}"
mkdir -p "${BUILD_DIR}/src" "${INSTALL_DIR}"
cp -a "${MUSL_SOURCE}/." "${BUILD_DIR}/src/"
rm -rf "${BUILD_DIR}/src/.git"

# The pinned toolchain, through the shims that carry its own runtime libraries
# (scripts/env.sh).
source "${ROOT_DIR}/scripts/env.sh"

# The pinned ABI (specs/build.md): hard-float rv64imafdc/lp64d.
CFLAGS="-march=rv64imafdc_zicsr_zifencei -mabi=lp64d -O2"

cd "${BUILD_DIR}"
./src/configure \
    --srcdir=src \
    --prefix="${INSTALL_DIR}" \
    --syslibdir="${INSTALL_DIR}/lib" \
    --target=riscv64 \
    --enable-static \
    --disable-shared \
    CC="riscv64-unknown-elf-gcc" \
    CROSS_COMPILE="riscv64-unknown-elf-" \
    CFLAGS="${CFLAGS}"

make -j"$(nproc)"
# install-libs and install-headers, not install: the latter also installs the
# dynamic linker and the musl-gcc wrapper, neither of which a static Aegir
# binary uses (sel4runtime is the entry point, specs/build.md).
make install-libs install-headers

echo "musl built for ${TARGET}: ${INSTALL_DIR}"
ls -la "${INSTALL_DIR}/lib/"
