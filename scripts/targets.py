"""The targets Aegir can build and boot.

Data, not logic: `scripts/run_target.py` does the work. Adding a target here is
how a new machine or configuration becomes runnable, and the architecture
specific values themselves live in configs/.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Target:
    name: str
    description: str
    build_dir: str
    # Flags passed to init-build.sh. Aegir's own target needs none: our pins
    # live in settings.cmake and configs/. Upstream targets have to be told,
    # which is a useful reminder that their defaults are not our decisions.
    configure_flags: tuple[str, ...]
    marker: str


TARGETS: dict[str, Target] = {
    "aegir": Target(
        name="aegir",
        description="Aegir's own root task",
        build_dir="out/aegir",
        configure_flags=(),
        marker="AEGIR_BOOT_OK",
    ),
    "sel4test": Target(
        name="sel4test",
        description="upstream seL4 test suite (acceptance test for the vendored kernel)",
        build_dir="out/sel4test",
        configure_flags=(
            "-DPLATFORM=qemu-riscv-virt",
            "-DCROSS_COMPILER_PREFIX=riscv64-unknown-elf-",
            "-DKernelRiscvExtD=ON",
            "-DSIMULATION=ON",
        ),
        marker="All is well in the universe",
    ),
}
