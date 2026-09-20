#!/usr/bin/env python3
"""Fetch Aegir's pinned tarball sources (manifests/sources.toml).

Most vendored trees come from git through repo (manifests/aegir.xml). The
tarball sources are pinned by sha256 and verified against the project's own
detached release signature before they are extracted into their project path.
musl is the first: its official git server is not a reliable fetch target
(git:// is blocked on some hosts, and the https URL is a web view, not a git
remote), and the release tarball is the canonical artifact.

    python3 scripts/fetch_sources.py           # fetch, verify, extract
    python3 scripts/fetch_sources.py --check    # verify only, offline

Exit status: 0 success, 1 failure.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path
from typing import Any

import pins


def verify_cached(source: dict[str, Any]) -> None:
    """Verify the cached tarball and signature against their pins, offline."""
    archive = pins.DOWNLOAD / source["archive"]
    signature = pins.DOWNLOAD / source["signature_archive"]
    if not archive.is_file() or pins.sha256_file(archive) != source["sha256"]:
        raise pins.PinError(f"{source['name']} tarball is missing or changed; run: make deps")
    if not signature.is_file() or pins.sha256_file(signature) != source["signature_sha256"]:
        raise pins.PinError(f"{source['name']} signature is missing or changed; run: make deps")
    pins.verify_signature(
        archive, signature, pins.ROOT / source["signing_key"], source["signing_fingerprint"]
    )


def up_to_date(source: dict[str, Any]) -> bool:
    """True when the extracted tree was made from this exact tarball."""
    target = pins.ROOT / source["path"]
    stamp = pins.read_stamp(f"source-{source['name']}")
    return bool(stamp) and stamp.get("sha256") == source["sha256"] and target.is_dir()


def fetch(source: dict[str, Any]) -> None:
    archive = pins.fetch(source["url"], source["sha256"], source["archive"])
    signature = pins.fetch(
        source["signature_url"], source["signature_sha256"], source["signature_archive"]
    )
    pins.verify_signature(
        archive, signature, pins.ROOT / source["signing_key"], source["signing_fingerprint"]
    )
    if up_to_date(source):
        pins.report(True, f"{source['name']} source is up to date", source["path"])
        return

    target = pins.ROOT / source["path"]
    extracted = pins.extract(archive, target.parent)
    if extracted != target:
        if target.exists():
            shutil.rmtree(target)
        extracted.rename(target)
    pins.write_stamp(
        f"source-{source['name']}", {"sha256": source["sha256"], "path": source["path"]}
    )
    pins.report(True, f"{source['name']} {source['version']} fetched and verified", source["path"])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="verify only, change nothing")
    arguments = parser.parse_args(argv)
    try:
        sources = pins.load_sources()
        if not sources:
            pins.report(True, "no tarball sources")
            return 0
        for source in sources:
            if arguments.check:
                if not up_to_date(source):
                    raise pins.PinError(
                        f"{source['path']} is not from its pinned tarball; run: make deps"
                    )
                verify_cached(source)
                pins.report(True, f"{source['name']} source verified", source["path"])
            else:
                fetch(source)
    except pins.PinError as exc:
        pins.report(False, "source fetch failed", str(exc))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
