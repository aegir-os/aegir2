#!/usr/bin/env python3
"""Fetch Aegir's pinned sources (manifests/sources.toml).

Most vendored trees come from git through repo (manifests/aegir.xml). The
sources here are the ones that do not: a tarball source, pinned by sha256 and
verified against the project's own detached release signature before it is
extracted (musl is the first -- its official git server is not a reliable fetch
target); and a file source, a list of individually pinned files placed under a
path (the Unicode Character Database, which publishes no release signature, so
the sha256 is the pin).

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


def is_file_source(source: dict[str, Any]) -> bool:
    """A file source pins a list of files instead of a tarball."""
    return "files" in source


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


def verify_file_source(source: dict[str, Any]) -> None:
    """Verify each pinned file's hash, offline."""
    target = pins.ROOT / source["path"]
    for entry in source["files"]:
        path = target / entry["name"]
        if not path.is_file() or pins.sha256_file(path) != entry["sha256"]:
            raise pins.PinError(f"{entry['name']} is missing or changed; run: make deps")


def up_to_date(source: dict[str, Any]) -> bool:
    """True when the fetched tree was made from this exact pin."""
    target = pins.ROOT / source["path"]
    if not target.is_dir():
        return False
    stamp = pins.read_stamp(f"source-{source['name']}")
    if not stamp:
        return False
    if is_file_source(source):
        return stamp.get("version") == source["version"] and all(
            (target / entry["name"]).is_file()
            and pins.sha256_file(target / entry["name"]) == entry["sha256"]
            for entry in source["files"]
        )
    return stamp.get("sha256") == source["sha256"]


def fetch_tarball(source: dict[str, Any]) -> None:
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


def fetch_file_source(source: dict[str, Any]) -> None:
    if up_to_date(source):
        pins.report(True, f"{source['name']} source is up to date", source["path"])
        return
    target = pins.ROOT / source["path"]
    for entry in source["files"]:
        # The download cache is flat, so a file pinned under a subdirectory
        # (extracted/...) keeps its name but lands at its pinned relative path.
        cached = pins.fetch(
            source["base_url"] + entry["name"], entry["sha256"], Path(entry["name"]).name
        )
        destination = target / entry["name"]
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(cached, destination)
    pins.write_stamp(
        f"source-{source['name']}", {"version": source["version"], "path": source["path"]}
    )
    pins.report(True, f"{source['name']} {source['version']} fetched and verified", source["path"])


def fetch(source: dict[str, Any]) -> None:
    if is_file_source(source):
        fetch_file_source(source)
    else:
        fetch_tarball(source)


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
                        f"{source['path']} is not from its pinned source; run: make deps"
                    )
                if is_file_source(source):
                    verify_file_source(source)
                else:
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
