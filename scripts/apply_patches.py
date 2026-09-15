#!/usr/bin/env python3
"""Apply Aegir's patches to the vendored trees.

Patches live in `third_party/patches/<project path>/…`. The directory layout
mirrors the vendored layout, so the project a patch belongs to is its directory
relative to `third_party/patches` (for example `kernel/`, or
`projects/musllibc/`). Anything outside a manifest project path is rejected.

Applying is idempotent: a patch already present in the tree is skipped, so this
can run after every `repo sync` without bookkeeping.

    python3 scripts/apply_patches.py            # apply what is missing
    python3 scripts/apply_patches.py --check    # verify only, change nothing

Exit status: 0 success, 1 failure, 2 usage error.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import pins


def is_applied(repository: Path, patch: Path) -> bool:
    """True if the tree already contains this patch."""
    return (
        pins.git(repository, "apply", "--reverse", "--check", str(patch), check=False).returncode
        == 0
    )


def applies_cleanly(repository: Path, patch: Path) -> bool:
    return pins.git(repository, "apply", "--check", str(patch), check=False).returncode == 0


def apply_all(check_only: bool) -> int:
    patches = pins.patches()
    if not patches:
        pins.report(True, "no patches to apply")
        return 0

    failures = 0
    for component, patch in patches:
        repository = pins.ROOT / component
        relative = patch.relative_to(pins.ROOT)
        if not repository.is_dir():
            pins.report(False, f"{relative} targets a missing project", component)
            failures += 1
            continue
        if is_applied(repository, patch):
            pins.report(True, f"{relative} already applied")
            continue
        if check_only:
            pins.report(False, f"{relative} is NOT applied", "run: make deps")
            failures += 1
            continue
        if not applies_cleanly(repository, patch):
            pins.report(
                False,
                f"{relative} does not apply to {component}",
                "the pinned revision changed, or the patch is stale",
            )
            failures += 1
            continue
        pins.git(repository, "apply", str(patch))
        pins.report(True, f"applied {relative}", component)

    return 1 if failures else 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check", action="store_true", help="verify patches are applied, change nothing"
    )
    arguments = parser.parse_args(argv)
    try:
        return apply_all(arguments.check)
    except pins.PinError as exc:
        pins.report(False, "patch application failed", str(exc))
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
