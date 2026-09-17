#!/usr/bin/env python3
"""Fetch the vendored seL4 tree from Aegir's pinned manifest.

Wraps `repo` so that the *tool itself* is pinned (repo downloads and can
self-update otherwise), the manifest comes from this repository, and the
fetching flags are the same every time.

    python3 scripts/sync_deps.py            # init + sync + record + patch
    python3 scripts/sync_deps.py --force    # ... discarding local changes
    python3 scripts/sync_deps.py --record-only

Environment:
    AEGIR_MANIFEST_URL  Override the manifest repository URL (default: this
                        working tree, as a file:// URL).
    AEGIR_MANIFEST_REV  Override the manifest repository revision (default: the
                        current branch of this working tree).

A full clone is fetched rather than a shallow one: our pinned revisions are
release-point commits that are not necessarily branch tips, and a shallow fetch
would not necessarily contain them. See specs/third_party.md.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

import pins

REPO = "repo"
SYNC_JOBS = str(max(1, (os.cpu_count() or 2) // 2))
PATCH_SCRIPT = pins.ROOT / "scripts" / "apply_patches.py"


def run(command: list[str], cwd: Path | None = None) -> None:
    print(f"INFO  {' '.join(command)}", flush=True)
    # stdin is /dev/null: `make deps` runs us under GNU timeout, which puts the
    # command in a background process group. repo prompts interactively when
    # stdin is a tty (git identity, color.ui) and that read then SIGTTIN-stops
    # the whole group -- a silent, permanent wedge. EOF skips the prompts and
    # turns any unexpected prompt into an error instead of a hang.
    subprocess.run(
        command, cwd=str(cwd or pins.ROOT), check=True, stdin=subprocess.DEVNULL
    )


def repo_tool_pin() -> dict[str, object]:
    pins_file = pins.load_pins()
    if "repo_tool" not in pins_file:
        raise pins.PinError("manifests/toolchain.toml has no [repo_tool] pin")
    return dict(pins_file["repo_tool"])


def manifest_source() -> tuple[str, str]:
    """Return (url, revision) for the manifest repository."""
    url = os.environ.get("AEGIR_MANIFEST_URL")
    if not url:
        url = pins.ROOT.as_uri()
    revision = os.environ.get("AEGIR_MANIFEST_REV")
    if not revision:
        revision = pins.git(pins.ROOT, "rev-parse", "--abbrev-ref", "HEAD").stdout.strip()
        if revision in ("", "HEAD"):
            raise pins.PinError(
                "cannot determine the manifest repository revision; "
                "set AEGIR_MANIFEST_REV"
            )
    return url, revision


def init(force: bool) -> None:
    tool = repo_tool_pin()
    url, revision = manifest_source()
    command = [
        REPO,
        "init",
        "--repo-url",
        str(tool["url"]),
        "--repo-rev",
        str(tool["rev"]),
        "--no-clone-bundle",
        "--manifest-url",
        url,
        "--manifest-branch",
        revision,
        "--manifest-name",
        str(pins.MANIFEST_AEGIR.relative_to(pins.ROOT)),
    ]
    run(command)
    if force:
        # A tree carrying applied patches is dirty by design; make it explicit.
        print("INFO  --force: `repo sync` will discard local changes", flush=True)


def sync(force: bool) -> None:
    command = [REPO, "sync", "--no-clone-bundle", "--jobs", SYNC_JOBS]
    if force:
        command.append("--force-sync")
    run(command)


def record() -> None:
    """Write the frozen record of what was actually fetched."""
    run([REPO, "manifest", "--revision", "--output", str(pins.MANIFEST_PINNED)])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--force",
        action="store_true",
        help="discard local changes in vendored trees (patches are re-applied)",
    )
    parser.add_argument(
        "--record-only",
        action="store_true",
        help="only regenerate manifests/aegir-pinned.xml from the current checkout",
    )
    arguments = parser.parse_args(argv)

    try:
        if not arguments.record_only:
            init(arguments.force)
            sync(arguments.force)
        record()
        if not arguments.record_only:
            run([sys.executable, str(PATCH_SCRIPT)])
    except (pins.PinError, subprocess.SubprocessError) as exc:
        pins.report(False, "dependency sync failed", str(exc))
        return 1

    pins.report(
        True,
        "vendored sources synced",
        f"{len(pins.load_projects())} projects at their pinned revisions",
    )
    print("INFO  run `make deps-check` to verify", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
