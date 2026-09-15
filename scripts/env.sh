# shellcheck shell=bash
#
# Put Aegir's pinned build tools on PATH:
#
#     . scripts/env.sh
#
# Then `cmake`, `ninja`, `dtc`, `protoc` and `riscv64-unknown-elf-*` resolve to
# the pinned copies. Nothing is installed outside the repository.

if [ -z "${BASH_VERSION:-}" ]; then
  echo "scripts/env.sh: needs bash (run it as: . scripts/env.sh)" >&2
fi

_aegir_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export AEGIR_ROOT="${AEGIR_ROOT:-$_aegir_root}"

# The cross toolchain is unpacked from Debian packages, so its layout is
# <prefix>/usr/bin. Its cc1 links libisl/libgmp/libmpfr/libmpc shared and the
# host does not necessarily have all of them, so the copies extracted with the
# toolchain go on LD_LIBRARY_PATH — but only for shells that sourced this file.
_aegir_toolchain_usr=$(find "$AEGIR_ROOT/third_party/toolchain" -maxdepth 2 -type d -name usr 2>/dev/null | head -n 1)
if [ -n "$_aegir_toolchain_usr" ]; then
  export PATH="$_aegir_toolchain_usr/bin:$PATH"
  _aegir_toolchain_libs="$_aegir_toolchain_usr/lib/x86_64-linux-gnu"
  if [ -d "$_aegir_toolchain_libs" ]; then
    export LD_LIBRARY_PATH="$_aegir_toolchain_libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  fi
else
  echo "scripts/env.sh: no RISC-V toolchain yet — run 'make tools'" >&2
fi

# cmake, ninja and the seL4 build's python dependencies.
if [ -x "$AEGIR_ROOT/third_party/tools/venv/bin/cmake" ]; then
  export PATH="$AEGIR_ROOT/third_party/tools/venv/bin:$PATH"
else
  echo "scripts/env.sh: no host build tools yet — run 'make tools'" >&2
fi

# Locally built host tools (dtc) and shims (protoc).
if [ -x "$AEGIR_ROOT/third_party/tools/bin/dtc" ]; then
  export PATH="$AEGIR_ROOT/third_party/tools/bin:$PATH"
else
  echo "scripts/env.sh: no dtc yet — run 'make tools'" >&2
fi

unset _aegir_root _aegir_toolchain_usr _aegir_toolchain_libs
