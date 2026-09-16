"""The targets Aegir can build and boot.

Data, not logic: `scripts/run_target.py` does the work. Adding a target here is
how a new machine or configuration becomes runnable, and the architecture
specific values themselves live in configs/.

Two of the values a target carries are *not* run-time flags: the amount of RAM
and the number of CPU cores are baked in at configure time, because the device
tree is dumped from QEMU with them and everything downstream reads that
(kernel/src/plat/qemu-riscv-virt/config.cmake:83-132). A different pair of
numbers is therefore a different build directory, which is why they appear as
configure flags here and as `-smp` in the simulate arguments -- and why the two
have to agree (specs/build.md).
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class Target:
    name: str
    description: str
    build_dir: str
    # Flags passed to init-build.sh. Aegir's own targets need none beyond the
    # machine they are for: our pins live in settings.cmake and configs/.
    # Upstream targets have to be told, which is a useful reminder that their
    # defaults are not our decisions.
    configure_flags: tuple[str, ...]
    marker: str
    # The project this target configures, relative to the repository root. Our
    # own targets configure the root project; an upstream project inside the tree
    # (projects/sel4test) has to be named, because the root's init-build.sh would
    # configure *our* root and the cached source directory would not match.
    source_dir: str = "."
    # Extra arguments handed to QEMU through the simulate script. The script has
    # no concept of cores, so `-smp` comes through here, and `-bios none` is its
    # own default repeated because passing anything replaces that default rather
    # than adding to it.
    qemu_args: tuple[str, ...] = field(default_factory=lambda: ("-bios none",))


def _aegir(memory_mib: int, cores: int, name: str) -> Target:
    """One point in the memory/cores envelope Aegir designs to (specs/aegir.md)."""
    return Target(
        name=name,
        description=f"Aegir's own root task -- {memory_mib} MiB, {cores} core(s)",
        build_dir=f"out/{name}",
        configure_flags=(f"-DQEMU_MEMORY={memory_mib}", f"-DKernelMaxNumNodes={cores}"),
        marker="AEGIR_BOOT_OK",
        # One real virtio device: an entropy source, the cheapest one because it
        # needs no backing file. Without a device, the transports the tree describes
        # are empty and answer nothing, which cannot show that a driver can read its
        # device at all (specs/services.md).
        # Two real virtio devices, so the transports the tree describes are not all
        # empty: an entropy source (which needs no backing file) and a block device
        # (which does -- the path is relative because QEMU runs with the build
        # directory as its working directory, and the runner puts a disk there).
        qemu_args=(
            "-bios none",
            f"-smp {cores}",
            "-device virtio-rng-device",
            "-drive file=disk.img,if=none,format=raw,id=hd",
            "-device virtio-blk-device,drive=hd",
        ),
    )


TARGETS: dict[str, Target] = {
    # The floor of the envelope: the smallest machine Aegir supports, and where
    # capacity problems are meant to show up first.
    "aegir": _aegir(2048, 1, "aegir"),
    # The rest of the QEMU matrix we care about, at the floor's memory and at the
    # upper end of the expected range.
    "aegir-2g-smp2": _aegir(2048, 2, "aegir-2g-smp2"),
    "aegir-2g-smp4": _aegir(2048, 4, "aegir-2g-smp4"),
    "aegir-8g-smp4": _aegir(8192, 4, "aegir-8g-smp4"),
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
        source_dir="projects/sel4test",
    ),
}

# The envelope, in the order it is worth trying: the floor first, then the same
# machine with more cores, then the upper end of the expected memory range.
ENVELOPE: tuple[str, ...] = ("aegir", "aegir-2g-smp2", "aegir-2g-smp4", "aegir-8g-smp4")
