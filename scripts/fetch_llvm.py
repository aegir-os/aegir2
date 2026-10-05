#!/usr/bin/env python3
"""Fetch and verify Aegir's pinned LLVM host toolchain.

Downloads the LLVM release named in `manifests/toolchain.toml`'s `[llvm]` pin
into an ignored cache, verifies its sha256 against the pin, and extracts it
under `third_party/toolchain/`. clang, lld and the LLVM binutils are then found
through `<prefix>/<release dir>/bin`, which `scripts/env.sh` puts on PATH.

    python3 scripts/fetch_llvm.py            # fetch/extract if needed
    python3 scripts/fetch_llvm.py --check    # verify only, no network

Unlike the GNU cross toolchain (fetch_toolchain.py), clang is self-contained:
there is no `cc1` to feed `libisl`/`libgmp`/`libmpfr`/`libmpc`, so no library
shims are needed. The one probe is the same idea as the GNU path's: prove the
compiler actually targets our ABI before a real build discovers it does not.

Exit status: 0 success, 1 pin/verification failure, 2 usage error.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import pins

COMMAND_TIMEOUT = 300
SMOKE_SOURCE = "int aegir_llvm_smoke(void) { return 0; }\n"

# The tools the Aegir build and the image flow reach through the clang prefix
# (specs/build.md). `clang` compiles and links; `ld.lld` is the linker clang
# invokes; the `llvm-*` tools replace GNU binutils in the image flow.
REQUIRED_TOOLS = (
    "clang",
    "clang++",
    "ld.lld",
    "llvm-objcopy",
    "llvm-readelf",
    "llvm-ar",
    "llvm-ranlib",
    "llvm-nm",
    "llvm-strip",
)

# The target Aegir builds for, matching configs/ and specs/build.md: hard-float
# lp64d on the qemu-riscv-virt machine.
TARGET = "riscv64-unknown-elf"
MARCH = "rv64imafdc_zicsr_zifencei"
MABI = "lp64d"
REQUIRE_FLOAT_ABI = "__riscv_float_abi_double"
REQUIRE_WORD_SIZE = "__riscv_xlen 64"


def load_llvm_pin() -> dict[str, object]:
    pins_file = pins.load_pins()
    if "llvm" not in pins_file:
        raise pins.PinError("manifests/toolchain.toml has no [llvm] pin")
    return dict(pins_file["llvm"])


def tool_prefix(pin: dict[str, object]) -> Path:
    return pins.TOOLCHAIN_ROOT / str(pin["prefix"])


def tool_bin(pin: dict[str, object]) -> Path:
    return tool_prefix(pin) / "bin"


def run_capture(command: list[str], stdin_text: str | None = None) -> str:
    completed = subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
        timeout=COMMAND_TIMEOUT,
        input=stdin_text,
    )
    return completed.stdout.strip()


def abi_flags() -> list[str]:
    return [f"--target={TARGET}", f"-march={MARCH}", f"-mabi={MABI}"]


def check_abi(pin: dict[str, object]) -> str:
    """Prove clang produces our exact ABI, from its predefined macros."""
    clang = tool_bin(pin) / "clang"
    macros = run_capture([str(clang), *abi_flags(), "-dM", "-E", "-"], stdin_text="")
    for required in (REQUIRE_FLOAT_ABI, REQUIRE_WORD_SIZE):
        if required not in macros:
            raise pins.PinError(f"{MARCH}/{MABI} does not define {required}")
    return f"{MARCH}/{MABI}"


def check_compiles(pin: dict[str, object]) -> None:
    """Compile one freestanding object for the target, and read its header."""
    clang = tool_bin(pin) / "clang"
    with tempfile.TemporaryDirectory(prefix="aegir-llvm-") as tmp:
        workdir = Path(tmp)
        source = workdir / "smoke.c"
        source.write_text(SMOKE_SOURCE, encoding="utf-8")
        obj = workdir / "smoke.o"
        run_capture([str(clang), *abi_flags(), "-ffreestanding", "-c", "-o", str(obj), str(source)])
        if not obj.is_file():
            raise pins.PinError("clang compiled nothing")
        header = run_capture([str(tool_bin(pin) / "llvm-readelf"), "-h", str(obj)])
        if "RISC-V" not in header:
            raise pins.PinError("the object clang produced is not RISC-V")


def check() -> int:
    pin = load_llvm_pin()
    name = str(pin["name"])
    version = str(pin["version"])
    prefix = tool_prefix(pin)
    if not prefix.is_dir():
        pins.report(False, f"{name} {version} is not installed", "run: make tools")
        return 1

    missing = [tool for tool in REQUIRED_TOOLS if not (tool_bin(pin) / tool).is_file()]
    if missing:
        pins.report(False, f"{name}: tools are missing", ", ".join(missing))
        return 1

    stamp = pins.read_stamp(name)
    if (
        stamp is None
        or stamp.get("version") != version
        or stamp.get("sha256") != pin["sha256"]
    ):
        pins.report(False, f"{name} stamp does not match the pin", f"pin {version}")
        return 1

    try:
        clang_version = run_capture([str(tool_bin(pin) / "clang"), "--version"]).splitlines()[0]
        abi = check_abi(pin)
        check_compiles(pin)
    except (pins.PinError, OSError, subprocess.SubprocessError) as exc:
        pins.report(False, f"{name} cannot build for our target", str(exc))
        return 1

    pins.report(True, f"{name} {version} present", f"{abi}; {clang_version}")
    return 0


def fetch(pin: dict[str, object]) -> None:
    archive = pins.fetch(
        str(pin["url"]), str(pin["sha256"]), str(pin["archive"])
    )
    extracted = pins.extract(archive, pins.TOOLCHAIN_ROOT)
    if extracted.name != str(pin["prefix"]):
        raise pins.PinError(
            f"the archive extracted to {extracted.name!r}, expected {pin['prefix']!r}"
        )
    pins.write_stamp(
        str(pin["name"]),
        {
            "name": pin["name"],
            "version": pin["version"],
            "sha256": pin["sha256"],
            "url": pin["url"],
            "prefix": pin["prefix"],
            "path": str(extracted.relative_to(pins.ROOT)),
        },
    )
    print(f"INFO  installed {extracted.relative_to(pins.ROOT)}", flush=True)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the installed toolchain without fetching anything",
    )
    arguments = parser.parse_args(argv)
    if arguments.check:
        return check()

    try:
        pin = load_llvm_pin()
        prefix = tool_prefix(pin)
        stamp = pins.read_stamp(str(pin["name"]))
        up_to_date = (
            prefix.is_dir()
            and stamp is not None
            and stamp.get("version") == pin["version"]
            and stamp.get("sha256") == pin["sha256"]
        )
        if not up_to_date:
            fetch(pin)
    except (pins.PinError, OSError, subprocess.SubprocessError) as exc:
        pins.report(False, "LLVM toolchain fetch failed", str(exc))
        return 1
    return check()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
