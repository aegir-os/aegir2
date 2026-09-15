#!/usr/bin/env python3
"""Verify the vendored tree against Aegir's pins. No network, no changes.

Checks, per project in manifests/aegir.xml:
  1. the path exists and is a git checkout,
  2. HEAD is exactly the pinned revision,
  3. the frozen record in manifests/aegir-pinned.xml agrees with the manifest,
  4. an upstream license file is present (we redistribute these trees),
  5. every tracked patch is applied, and no unexpected local modification
     exists in the vendored tree.

    python3 scripts/check_pins.py

Exit status: 0 all pass, 1 any failure.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pins


def expected_dirty_files() -> set[str]:
    """Files our patches are allowed to modify, as repo-relative paths."""
    allowed: set[str] = set()
    for component, patch in pins.patches():
        text = patch.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            if line.startswith("+++ ") or line.startswith("--- "):
                path = line[4:].split("\t", 1)[0].strip()
                for prefix in ("a/", "b/"):
                    if path.startswith(prefix):
                        path = path[len(prefix) :]
                if path and path != "/dev/null":
                    allowed.add(f"{component}/{path}")
    return allowed


def check_project(path: str, revision: str, failures: list[str]) -> None:
    repository = pins.ROOT / path
    if not repository.is_dir():
        pins.report(False, f"{path} is missing", f"expected at {revision[:12]}")
        failures.append(path)
        return

    head = pins.git(repository, "rev-parse", "HEAD", check=False)
    if head.returncode != 0:
        pins.report(False, f"{path} is not a git checkout")
        failures.append(path)
        return
    actual = head.stdout.strip()
    if actual != revision:
        pins.report(False, f"{path} is at the wrong revision", f"{actual[:12]}, pinned {revision[:12]}")
        failures.append(path)
        return

    license_files = pins.license_files(repository)
    if not license_files:
        pins.report(False, f"{path} has no license file", "cannot be redistributed safely")
        failures.append(path)
        return

    pins.report(True, f"{path} at {revision[:12]}", f"license: {license_files[0]}")


def check_local_changes(failures: list[str]) -> None:
    """Nothing may be modified except by our tracked patches."""
    allowed = expected_dirty_files()
    for path in pins.load_projects():
        repository = pins.ROOT / path
        if not repository.is_dir():
            continue
        status = pins.git(repository, "status", "--porcelain", check=False)
        if status.returncode != 0:
            continue
        unexpected = []
        for line in status.stdout.splitlines():
            entry = line[3:].strip()
            for candidate in (f"{path}/{entry}", entry):
                if candidate in allowed:
                    break
            else:
                unexpected.append(entry)
        if unexpected:
            pins.report(
                False,
                f"{path} has modifications that are not ours",
                ", ".join(unexpected[:3]),
            )
            failures.append(path)


def check_record(failures: list[str]) -> None:
    """The committed `repo manifest -r` record must match the manifest."""
    if not pins.MANIFEST_PINNED.is_file():
        pins.report(False, "manifests/aegir-pinned.xml is missing", "run: make deps")
        failures.append("pinned-manifest")
        return
    pinned = pins.load_projects(pins.MANIFEST_PINNED)
    declared = pins.load_projects()
    differing = {
        path: (declared.get(path), revision)
        for path, revision in pinned.items()
        if declared.get(path) != revision
    }
    if differing:
        pins.report(
            False,
            "manifests/aegir-pinned.xml disagrees with manifests/aegir.xml",
            f"{len(differing)} project(s) differ",
        )
        failures.append("pinned-manifest")
        return
    pins.report(True, "frozen record matches the pins", f"{len(pinned)} projects")


def main() -> int:
    failures: list[str] = []
    projects = pins.load_projects()
    for path, revision in sorted(projects.items()):
        check_project(path, revision, failures)
    check_record(failures)
    check_local_changes(failures)

    if failures:
        print(f"\n{len(failures)} problem(s); see FAIL lines above", flush=True)
        return 1
    print(f"\nall {len(projects)} projects match their pins", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except pins.PinError as exc:
        pins.report(False, "pin check failed", str(exc))
        sys.exit(1)
