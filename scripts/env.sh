# shellcheck shell=bash
#
# Put Aegir's pinned build tools on PATH:
#
#     . scripts/env.sh
#
# Then `cmake`, `ninja` and `riscv-none-elf-*` resolve to the versions pinned in
# manifests/toolchain.toml and manifests/requirements-tools.txt. Nothing is
# installed outside the repository.

if [ -z "${BASH_VERSION:-}" ]; then
  echo "scripts/env.sh: needs bash (run it as: . scripts/env.sh)" >&2
fi

_aegir_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export AEGIR_ROOT="${AEGIR_ROOT:-$_aegir_root}"

_aegir_toolchain_bin=$(find "$AEGIR_ROOT/third_party/toolchain" -maxdepth 2 -type d -name bin 2>/dev/null | head -n 1)
if [ -n "$_aegir_toolchain_bin" ]; then
  export PATH="$_aegir_toolchain_bin:$PATH"
else
  echo "scripts/env.sh: no RISC-V toolchain yet — run 'make tools'" >&2
fi

if [ -x "$AEGIR_ROOT/third_party/tools/venv/bin/cmake" ]; then
  export PATH="$AEGIR_ROOT/third_party/tools/venv/bin:$PATH"
else
  echo "scripts/env.sh: no host build tools yet — run 'make tools'" >&2
fi

unset _aegir_root _aegir_toolchain_bin
