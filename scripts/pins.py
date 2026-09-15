"""Shared helpers for Aegir's host tooling.

Knows where pinned third-party artifacts live, how to hash a file, and how a
fetch/install records itself in a stamp file. Stdlib only, so these scripts run
on a bare host with nothing but python3.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
import tomllib
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
MANIFESTS = ROOT / "manifests"
THIRD_PARTY = ROOT / "third_party"
DOWNLOAD = THIRD_PARTY / "download"
TOOLCHAIN_ROOT = THIRD_PARTY / "toolchain"
TOOLS_ROOT = THIRD_PARTY / "tools"
PATCH_ROOT = THIRD_PARTY / "patches"

PINS_FILE = MANIFESTS / "toolchain.toml"
REQUIREMENTS_FILE = MANIFESTS / "requirements-tools.txt"
MANIFEST_AEGIR = MANIFESTS / "aegir.xml"
MANIFEST_PINNED = MANIFESTS / "aegir-pinned.xml"

# License files we expect to find at a vendored project's root. Used only to
# confirm a tree is intact enough to redistribute; the authoritative texts are
# the LICENSES/ directories inside each tree.
LICENSE_NAMES = (
    "LICENSE",
    "LICENSE.md",
    "LICENSE.txt",
    "LICENSES",
    "COPYING",
    "COPYRIGHT",
)

CHUNK = 1 << 20


class PinError(RuntimeError):
    """A pinned input is missing, unverified or in an unexpected state."""


def load_pins() -> dict[str, Any]:
    """Return the parsed pin file (manifests/toolchain.toml)."""
    try:
        with PINS_FILE.open("rb") as handle:
            return tomllib.load(handle)
    except FileNotFoundError as exc:
        raise PinError(f"missing pin file: {PINS_FILE}") from exc


def sha256_file(path: Path) -> str:
    """Return the hex sha256 of a file, streaming so size does not matter."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stamp_path(name: str) -> Path:
    return TOOLCHAIN_ROOT / f".{name}.stamp"


def read_stamp(name: str) -> dict[str, Any] | None:
    path = stamp_path(name)
    if not path.is_file():
        return None
    try:
        with path.open("r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, json.JSONDecodeError):
        return None
    return data if isinstance(data, dict) else None


def write_stamp(name: str, fields: dict[str, Any]) -> None:
    path = stamp_path(name)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(fields, handle, indent=2, sort_keys=True)
        handle.write("\n")


def report(ok: bool, what: str, detail: str = "") -> None:
    status = "PASS" if ok else "FAIL"
    suffix = f"  ({detail})" if detail else ""
    print(f"{status}  {what}{suffix}", flush=True)


def load_projects(manifest: Path = MANIFEST_AEGIR) -> dict[str, str]:
    """Return {project path: pinned revision} from a repo manifest."""
    try:
        tree = ElementTree.parse(manifest)
    except (OSError, ElementTree.ParseError) as exc:
        raise PinError(f"cannot read manifest {manifest}: {exc}") from exc

    projects: dict[str, str] = {}
    for element in tree.getroot().findall("project"):
        path = element.get("path")
        revision = element.get("revision")
        if not path or not revision:
            raise PinError(f"manifest {manifest}: project without path/revision")
        projects[path] = revision
    if not projects:
        raise PinError(f"manifest {manifest}: no projects")
    return projects


def patches() -> list[tuple[str, Path]]:
    """Return [(vendored project path, patch file)] for our tracked patches.

    Patch directory names mirror the vendored layout
    (`third_party/patches/kernel/x.patch`, `third_party/patches/projects/musllibc/y.patch`),
    so a patch's component is its directory relative to third_party/patches.
    """
    if not PATCH_ROOT.is_dir():
        return []
    known = set(load_projects())
    found: list[tuple[str, Path]] = []
    for patch in sorted(PATCH_ROOT.rglob("*.patch")):
        component = patch.parent.relative_to(PATCH_ROOT).as_posix()
        if component not in known:
            raise PinError(
                f"patch {patch.relative_to(ROOT)} is not under a vendored project "
                f"path ({component!r} is not in the manifest)"
            )
        found.append((component, patch))
    return found


def git(repository: Path, *arguments: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    """Run git in a repository and return the completed process."""
    return subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=check,
        capture_output=True,
        text=True,
        timeout=120,
    )
