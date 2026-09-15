#!/usr/bin/env python3
"""Install Aegir's pinned host build tools into a private virtualenv.

cmake and ninja are not assumed to exist on the host. They are installed into
`third_party/tools/venv` from the wheels pinned (version *and* sha256) in
`manifests/requirements-tools.txt`, so nothing is installed system-wide.

    python3 scripts/setup_tools.py            # create/update the venv
    python3 scripts/setup_tools.py --check    # verify only, no network

Exit status: 0 success, 1 failure, 2 usage error.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import venv
from pathlib import Path

import pins

VENV = pins.TOOLS_ROOT / "venv"
STAMP_NAME = "host-tools"
REQUIRED_BINARIES = ("cmake", "ninja")


def pip_install(requirements: Path) -> None:
    pip = VENV / "bin" / "pip"
    command = [
        str(pip),
        "install",
        "--disable-pip-version-check",
        "--no-cache-dir",
        "--require-hashes",
        "--requirement",
        str(requirements),
    ]
    print(f"INFO  {' '.join(command)}", flush=True)
    subprocess.run(command, check=True, timeout=900)


def check() -> int:
    stamp = pins.read_stamp(STAMP_NAME)
    if stamp is None:
        pins.report(False, "host build tools are not installed", "run: make tools")
        return 1
    if stamp.get("requirements_sha256") != pins.sha256_file(pins.REQUIREMENTS_FILE):
        pins.report(
            False,
            "installed host tools do not match manifests/requirements-tools.txt",
            "re-run: make tools",
        )
        return 1
    for binary in REQUIRED_BINARIES:
        path = VENV / "bin" / binary
        if not path.is_file():
            pins.report(False, f"missing {path.relative_to(pins.ROOT)}")
            return 1
    pins.report(
        True,
        "host build tools present",
        ", ".join(f"{name} {stamp['versions'].get(name, '?')}" for name in REQUIRED_BINARIES),
    )
    return 0


def versions() -> dict[str, str]:
    """Ask each installed tool for its version, to record in the stamp."""
    found: dict[str, str] = {}
    for binary in REQUIRED_BINARIES:
        output = subprocess.run(
            [str(VENV / "bin" / binary), "--version"],
            check=True,
            capture_output=True,
            text=True,
            timeout=60,
        ).stdout
        found[binary] = output.splitlines()[0].strip()
    return found


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check", action="store_true", help="verify without installing anything"
    )
    arguments = parser.parse_args(argv)

    if arguments.check:
        return check()

    try:
        if not (VENV / "bin" / "python").exists():
            print(f"INFO  creating virtualenv {VENV.relative_to(pins.ROOT)}", flush=True)
            VENV.parent.mkdir(parents=True, exist_ok=True)
            venv.EnvBuilder(with_pip=True, clear=False).create(VENV)
        pip_install(pins.REQUIREMENTS_FILE)
        pins.write_stamp(
            STAMP_NAME,
            {
                "name": STAMP_NAME,
                "requirements": str(pins.REQUIREMENTS_FILE.relative_to(pins.ROOT)),
                "requirements_sha256": pins.sha256_file(pins.REQUIREMENTS_FILE),
                "versions": versions(),
            },
        )
    except (OSError, subprocess.SubprocessError) as exc:
        pins.report(False, "host tool install failed", str(exc))
        return 1
    return check()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
