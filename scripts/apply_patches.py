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


def is_applied(component: str, patch: Path) -> bool:
    """True if the tree already contains this patch.

    The patch is applied from the Aegir root with `--directory=<component>`,
    not from inside the vendored tree. A component that is not its own git
    repository (a tree extracted without its repo-tool gitdir, say) would
    otherwise make `git apply` walk up to Aegir's repository and silently do
    nothing; applying from a repository we know is real, with the component as
    a prefix, is the same edit either way and never no-ops.
    """
    return (
        pins.git(
            pins.ROOT,
            "apply",
            "--directory",
            component,
            "--reverse",
            "--check",
            str(patch),
            check=False,
        ).returncode
        == 0
    )


def applies_cleanly(component: str, patch: Path) -> bool:
    return (
        pins.git(
            pins.ROOT, "apply", "--directory", component, "--check", str(patch), check=False
        ).returncode
        == 0
    )


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
        if is_applied(component, patch):
            pins.report(True, f"{relative} already applied")
            continue
        if check_only:
            pins.report(False, f"{relative} is NOT applied", "run: make deps")
            failures += 1
            continue
        if not applies_cleanly(component, patch):
            pins.report(
                False,
                f"{relative} does not apply to {component}",
                "the pinned revision changed, or the patch is stale",
            )
            failures += 1
            continue
        pins.git(pins.ROOT, "apply", "--directory", component, str(patch))
        pins.report(True, f"applied {relative}", component)

    return 1 if failures else 0


def revert_all() -> int:
    """Undo the Aegir patches this tree carries, so a vendored tree is clean at
    its pin again.

    An applied patch is a local change, and `repo sync` refuses to check out a
    pinned revision over one -- so `make deps` re-runs by reversing the patches
    first and re-applying them after the sync (they are Aegir's, not the
    vendored projects'). A patch that is not applied, or a project that is not
    there yet, is skipped."""
    failures = 0
    for component, patch in pins.patches():
        repository = pins.ROOT / component
        relative = patch.relative_to(pins.ROOT)
        if not repository.is_dir() or not is_applied(component, patch):
            continue
        result = pins.git(
            pins.ROOT, "apply", "--reverse", "--directory", component, str(patch),
            check=False,
        )
        if result.returncode != 0:
            pins.report(False, f"{relative} could not be reversed", component)
            failures += 1
        else:
            pins.report(True, f"reversed {relative}", component)
    return 1 if failures else 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check", action="store_true", help="verify patches are applied, change nothing"
    )
    parser.add_argument(
        "--reverse",
        action="store_true",
        help="undo the patches this tree carries, so `repo sync` can re-run",
    )
    arguments = parser.parse_args(argv)
    try:
        if arguments.reverse:
            return revert_all()
        return apply_all(arguments.check)
    except pins.PinError as exc:
        pins.report(False, "patch application failed", str(exc))
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
