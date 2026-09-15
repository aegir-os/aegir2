# Third-party inventory

Aegir commits **no** third-party source. Every component below is fetched on
demand at the revision pinned in [`manifests/aegir.xml`](manifests/aegir.xml)
into a gitignored path, then patched (if we have any patches) by
`scripts/apply-patches`. `make deps-check` re-verifies the pins.

The revisions are the **seL4 16.0.0 release set**, taken verbatim from the
upstream release manifest (`manifests/upstream-16.0.0.xml`, from
`seL4/sel4test-manifest` tag `16.0.0`).

| Path | Upstream | SPDX license | Role |
| --- | --- | --- | --- |
| `kernel/` | [seL4/seL4](https://github.com/seL4/seL4) | `GPL-2.0-only` | The microkernel (privileged); shipped as a separate program |
| `tools/seL4/` | [seL4/seL4_tools](https://github.com/seL4/seL4_tools) | `BSD-2-Clause` | CMake build system + ELF loader (`elfloader-tool`) |
| `projects/musllibc/` | [seL4/musllibc](https://github.com/seL4/musllibc) | `MIT` | C standard library for userland (seL4 `sel4` branch) |
| `projects/sel4runtime/` | [seL4/sel4runtime](https://github.com/seL4/sel4runtime) | `BSD-2-Clause` | C runtime / entry point for userland |
| `projects/util_libs/` | [seL4/util_libs](https://github.com/seL4/util_libs) | `BSD-2-Clause` | Platform support libraries (`libplatsupport`, …) |
| `projects/seL4_libs/` | [seL4/seL4_libs](https://github.com/seL4/seL4_libs) | `BSD-2-Clause` | libsel4* convenience libraries (see below) |
| `projects/sel4_projects_libs/` | [seL4/sel4_projects_libs](https://github.com/seL4/sel4_projects_libs) | `BSD-2-Clause` | Additional project libraries |
| `projects/sel4test/` | [seL4/sel4test](https://github.com/seL4/sel4test) | `BSD-2-Clause` | Upstream kernel test suite (our end-to-end acceptance test) |
| `tools/opensbi/` | [riscv/opensbi](https://github.com/riscv/opensbi) | `BSD-2-Clause` | RISC-V firmware, used by the RISC-V image flow |
| `tools/nanopb/` | [nanopb](https://github.com/nanopb/nanopb) | `Zlib` | Protocol buffers, pulled in by the release manifest |

`libsel4` — the userspace bindings to the kernel ABI — is `BSD-2-Clause` and is
generated from `kernel/` into the build directory; that is what makes an
MIT-licensed OS on a GPL-2.0 kernel legitimate (see `specs/third_party.md`).

The `SPDX license` column records the license the upstream repositories declare
for their code. `scripts/check_pins.py` asserts each vendored tree still carries
a license file at its root, and the license texts shipped in a release image
come from the `LICENSES/` directories inside those trees.

Two precisions, verified against the pinned trees (2026-09-15):

- **seL4's own `LICENSE.md` files are `CC-BY-SA-4.0`** (they are documentation).
  The authoritative statement for each file is its SPDX header; `kernel/`
  declares "generally, kernel-level code is licensed under GPLv2 and user-level
  code under the 2-clause BSD license". Both statements are true at once, and
  that split is precisely what makes an MIT Aegir possible.
- **musllibc is MIT** (musl's `COPYRIGHT`: "musl as a whole is licensed under
  the following standard MIT license"), and the tree is seL4's `sel4` branch,
  pinned to the commit on that branch.

## Build-time tools (not distributed)

Fetched and pinned by `make tools`, but only used to *produce* an image; none of
it is linked into or shipped with Aegir:

| Tool | Pin | License |
| --- | --- | --- |
| `riscv64-unknown-elf-gcc` + binutils | `manifests/toolchain.toml` (Debian packages, sha256 from Debian's signed index) | GPL-3.0-or-later (compiler), GPL-2.0-or-later (binutils) |
| its runtime libs (libisl, libgmp, libmpfr, libmpc) | same file | LGPL/MIT-style per package |
| `dtc` | `manifests/toolchain.toml` (kernel.org release, sha256 from the project's signed sums) | GPL-2.0-or-later OR BSD-2-Clause |
| `cmake`, `ninja`, seL4's Python dependencies | `manifests/tools-declared.txt` → `manifests/requirements-tools.txt`, installed with `pip --require-hashes` | Apache-2.0 / Apache-2.0 / per package |
| `repo` | `manifests/toolchain.toml` `[repo_tool]` (commit pin) | Apache-2.0 |

Using GCC to compile Aegir's own code has no effect on Aegir's license: the GCC
Runtime Library Exception covers `libgcc`, and the compiler is a tool, not a
combined work.
