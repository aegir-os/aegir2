#!/usr/bin/env python3
"""Build the pinned `dtc` (device tree compiler) into the workspace.

seL4's qemu-riscv-virt build needs `dtc` in both directions: it converts a
device tree dumped from QEMU into DTS at configure time, and compiles DTS back
into a DTB for the ELF loader. The host has no `dtc` and no way to install one
(no root), so it is built from the pinned kernel.org release tarball.

    python3 scripts/build_dtc.py            # fetch, build, install if needed
    python3 scripts/build_dtc.py --check    # verify only, no network/build

The binary is installed at third_party/tools/bin/dtc, which scripts/env.sh puts
on PATH. Exit status: 0 success, 1 failure, 2 usage error.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pins

BUILD_ROOT = pins.TOOLS_ROOT / "src"
BIN_ROOT = pins.TOOLS_ROOT / "bin"
STAMP_NAME = "dtc"
# We need the compiler only; python bindings, valgrind wrappers and libyaml
# support are irrelevant here (and libyaml may not be installed).
MAKE_VARIABLES = ("NO_PYTHON=1", "NO_YAML=1", "NO_VALGRIND=1")
SMOKE_DTS = """
/dts-v1/;

/ {
\taegir,smoke-test;
\tsubnode {
\t\taegir,value = <0x2a>;
\t};
};
"""


def make_jobs() -> str:
    return str(max(1, (os.cpu_count() or 2) // 2))


def build(pin: dict[str, object], source: Path) -> None:
    command = [
        "make",
        "-C",
        str(source),
        "-j",
        make_jobs(),
        *MAKE_VARIABLES,
        "dtc",
    ]
    # dtc's Makefile runs `git describe` when it can find a repository. The
    # extracted tree has none, so git would walk up into Aegir's own working
    # tree and stamp our HEAD and dirty state into the binary
    # (observed: "1.8.1-g388493ba-dirty"). Setting a ceiling keeps the build
    # hermetic, so the same pinned tarball always yields the same version.
    environment = os.environ.copy()
    environment["GIT_CEILING_DIRECTORIES"] = str(source.parent)
    print(f"INFO  {' '.join(command)}", flush=True)
    subprocess.run(command, check=True, timeout=600, env=environment)


def install(source: Path) -> None:
    built = source / "dtc"
    if not built.is_file():
        raise pins.PinError(f"build produced no dtc at {built}")
    BIN_ROOT.mkdir(parents=True, exist_ok=True)
    shutil.copy2(built, BIN_ROOT / "dtc")


def check(pin: dict[str, object]) -> int:
    name = str(pin["name"])
    stamp = pins.read_stamp(STAMP_NAME)
    if stamp is None:
        pins.report(False, f"{name} is not installed", "run: make tools")
        return 1
    if stamp.get("sha256") != pin["sha256"] or stamp.get("version") != pin["version"]:
        pins.report(False, f"{name} stamp does not match the pin", "re-run: make tools")
        return 1

    dtc = BIN_ROOT / "dtc"
    if not dtc.is_file():
        pins.report(False, f"missing {dtc.relative_to(pins.ROOT)}")
        return 1

    try:
        version = subprocess.run(
            [str(dtc), "--version"],
            check=True,
            capture_output=True,
            text=True,
            timeout=60,
        ).stdout.strip()
        roundtrip(dtc)
    except (OSError, subprocess.SubprocessError) as exc:
        pins.report(False, f"{name} is not usable", str(exc))
        return 1

    reported = version.split()[-1] if version else "unknown"
    # dtc appends its own git describe suffix when it can see a repository, so
    # compare the version it is based on rather than demanding an exact match.
    if reported != str(pin["version"]) and not reported.startswith(f"{pin['version']}-"):
        pins.report(False, f"{name} reports {reported}, pinned {pin['version']}")
        return 1
    pins.report(True, f"{name} {pin['version']} present", "dts->dtb->dts roundtrip ok")
    return 0


def roundtrip(dtc: Path) -> None:
    """Prove the binary actually works: compile a tree, then read it back."""
    with tempfile.TemporaryDirectory(prefix="aegir-dtc-") as tmp:
        workdir = Path(tmp)
        source = workdir / "smoke.dts"
        source.write_text(SMOKE_DTS, encoding="utf-8")
        blob = workdir / "smoke.dtb"
        subprocess.run(
            [str(dtc), "-q", "-I", "dts", "-O", "dtb", "-o", str(blob), str(source)],
            check=True,
            capture_output=True,
            text=True,
            timeout=60,
        )
        text = subprocess.run(
            [str(dtc), "-q", "-I", "dtb", "-O", "dts", str(blob)],
            check=True,
            capture_output=True,
            text=True,
            timeout=60,
        ).stdout
        if "aegir,smoke-test" not in text:
            raise pins.PinError("dtc roundtrip lost the test property")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check", action="store_true", help="verify without fetching or building"
    )
    arguments = parser.parse_args(argv)

    pin = dict(pins.load_pins()["dtc"])
    if arguments.check:
        return check(pin)

    stamp = pins.read_stamp(STAMP_NAME)
    if (
        stamp is not None
        and stamp.get("sha256") == pin["sha256"]
        and stamp.get("version") == pin["version"]
        and check(pin) == 0
    ):
        return 0

    try:
        archive = pins.fetch(str(pin["url"]), str(pin["sha256"]), str(pin["archive"]))
        source = pins.extract(archive, BUILD_ROOT)
        build(pin, source)
        install(source)
        pins.write_stamp(
            STAMP_NAME,
            {
                "name": pin["name"],
                "version": pin["version"],
                "sha256": pin["sha256"],
                "url": pin["url"],
                "path": str((BIN_ROOT / "dtc").relative_to(pins.ROOT)),
            },
        )
    except (pins.PinError, OSError, subprocess.SubprocessError) as exc:
        pins.report(False, "dtc build failed", str(exc))
        return 1
    return check(pin)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
