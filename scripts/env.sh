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
# The root is where this file lives — never whatever AEGIR_ROOT the shell
# happens to carry. A stale export (an old checkout, a profile leftover) makes
# every check below look in the wrong tree and claim the tools are missing
# while `make tools` — which resolves its own path — correctly finds them.
if [ -n "${AEGIR_ROOT:-}" ] && [ "$AEGIR_ROOT" != "$_aegir_root" ]; then
  echo "scripts/env.sh: ignoring AEGIR_ROOT=$AEGIR_ROOT (this tree is $_aegir_root)" >&2
fi
export AEGIR_ROOT="$_aegir_root"

# The cross toolchain is unpacked from Debian packages, so its layout is
# <prefix>/usr/bin. Its cc1 links libisl/libgmp/libmpfr/libmpc shared and the
# host does not necessarily have all of them, so each tool is exposed through
# a shim in <prefix>/shims that puts the bundled copies on LD_LIBRARY_PATH for
# that process only (scripts/fetch_toolchain.py:make_shims). Exporting
# LD_LIBRARY_PATH here instead would leak those Debian library builds into
# every host process the build spawns — qemu loading the toolchain's libgmp
# hung `make build` at the kernel's dtb extraction on Fedora. The probe is a
# glob loop, not `find | head`: this file is sourced by callers running
# `set -euo pipefail`, and a head(1) that exits early can SIGPIPE the find,
# which pipefail then turns into a silent `set -e` death mid-source.
_aegir_toolchain_shims=""
for _aegir_candidate in "$AEGIR_ROOT"/third_party/toolchain/*/shims; do
  if [ -d "$_aegir_candidate" ]; then
    _aegir_toolchain_shims="$_aegir_candidate"
    break
  fi
done
if [ -n "$_aegir_toolchain_shims" ]; then
  export PATH="$_aegir_toolchain_shims:$PATH"
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

unset _aegir_root _aegir_candidate _aegir_toolchain_shims
