#!/usr/bin/env python3
"""Generate the hashed, fully pinned host-tool lock file.

manifests/tools-declared.txt is what we ask for; this resolves the transitive
closure with pip for the current interpreter and platform and writes
manifests/requirements-tools.txt with a sha256 for every artifact. That lock
file is what `scripts/setup_tools.py` installs, with --require-hashes, so a
substituted or tampered artifact fails the install.

    python3 scripts/lock_tools.py            # regenerate the lock
    python3 scripts/lock_tools.py --check    # verify the lock offline

Run this only when deliberately bumping a tool version, then commit the result.
Exit status: 0 success, 1 failure, 2 usage error.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pins

DECLARED = pins.MANIFESTS / "tools-declared.txt"
LOCK = pins.REQUIREMENTS_FILE
BINARY_SUFFIXES = (".whl", ".tar.gz", ".tar.bz2", ".tar.xz", ".zip")


def canonical(name: str) -> str:
    """PEP 503 name normalisation, so the lock matches what pip resolves."""
    return re.sub(r"[-_.]+", "-", name).lower()


def read_declared() -> dict[str, str]:
    """Return {canonical name: pinned version} from the declared file."""
    declared: dict[str, str] = {}
    for raw in DECLARED.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if "==" not in line:
            raise pins.PinError(
                f"{DECLARED.name}: {line!r} is not an exact 'name==version' pin"
            )
        name, version = line.split("==", 1)
        declared[canonical(name.strip())] = version.strip()
    if not declared:
        raise pins.PinError(f"{DECLARED.name} declares nothing")
    return declared


def parse_artifact(filename: str) -> tuple[str, str]:
    """Return (canonical name, version) for a wheel or sdist file name."""
    stem = filename
    for suffix in BINARY_SUFFIXES:
        if stem.endswith(suffix):
            stem = stem[: -len(suffix)]
            break
    parts = stem.split("-")
    if len(parts) < 2:
        raise pins.PinError(f"cannot parse artifact name: {filename}")
    return canonical(parts[0]), parts[1]


def resolve() -> dict[tuple[str, str], set[str]]:
    """Download the closure with pip and hash every artifact it fetched."""
    workdir = Path(tempfile.mkdtemp(prefix="aegir-lock-"))
    try:
        command = [
            sys.executable,
            "-m",
            "pip",
            "download",
            "--dest",
            str(workdir),
            "--no-cache-dir",
            "--disable-pip-version-check",
            "--requirement",
            str(DECLARED),
        ]
        print(f"INFO  {' '.join(command)}", flush=True)
        subprocess.run(command, check=True, timeout=900)
        resolved: dict[tuple[str, str], set[str]] = {}
        for artifact in sorted(workdir.iterdir()):
            if not artifact.is_file():
                continue
            key = parse_artifact(artifact.name)
            resolved.setdefault(key, set()).add(pins.sha256_file(artifact))
        return resolved
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def render(resolved: dict[tuple[str, str], set[str]]) -> str:
    lines = [
        "# GENERATED FILE — do not edit.",
        "#",
        "# Written by scripts/lock_tools.py from manifests/tools-declared.txt.",
        "# Regenerate with `make lock-tools` after changing the declared pins.",
        "#",
        "# Every artifact is pinned by version and sha256 and installed with",
        "# pip --require-hashes. Hashes are for wheels resolved on linux-x86_64",
        "# with CPython 3.14; other hosts are not supported yet.",
        "",
    ]
    for (name, version), hashes in sorted(resolved.items()):
        digest_flags = " ".join(f"--hash=sha256:{digest}" for digest in sorted(hashes))
        lines.append(f"{name}=={version} {digest_flags}")
    return "\n".join(lines) + "\n"


def read_lock() -> dict[str, str]:
    """Return {canonical name: version} from the generated lock file."""
    locked: dict[str, str] = {}
    for raw in LOCK.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        name, _, rest = line.partition("==")
        version = rest.split(" ", 1)[0].strip()
        if not name or not version:
            raise pins.PinError(f"{LOCK.name}: cannot parse {line!r}")
        locked[canonical(name)] = version
    return locked


def check() -> int:
    if not LOCK.is_file():
        pins.report(False, f"{LOCK.name} is missing", "run: make lock-tools")
        return 1
    locked = read_lock()
    declared = read_declared()
    missing = {name: version for name, version in declared.items() if locked.get(name) != version}
    if missing:
        detail = ", ".join(f"{name}=={version}" for name, version in sorted(missing.items()))
        pins.report(False, f"{LOCK.name} does not match the declared pins", detail)
        return 1
    pins.report(
        True,
        f"{LOCK.name} matches the declared pins",
        f"{len(declared)} direct, {len(locked)} total packages",
    )
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the lock against the declared pins, without network access",
    )
    arguments = parser.parse_args(argv)

    if arguments.check:
        return check()

    try:
        resolved = resolve()
        LOCK.write_text(render(resolved), encoding="utf-8")
        pins.report(
            True,
            f"wrote {LOCK.relative_to(pins.ROOT)}",
            f"{len(resolved)} packages pinned by hash",
        )
    except (pins.PinError, OSError, subprocess.SubprocessError) as exc:
        pins.report(False, "lock generation failed", str(exc))
        return 1
    return check()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
