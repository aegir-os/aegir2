"""Shared helpers for Aegir's host tooling.

Knows where pinned third-party artifacts live, how to hash a file, and how a
fetch/install records itself in a stamp file. Stdlib only, so these scripts run
on a bare host with nothing but python3.
"""

from __future__ import annotations

import hashlib
import io
import json
import shutil
import subprocess
import tarfile
import tomllib
import urllib.error
import urllib.request
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

# Root-level license files of a vendored tree, matched by name prefix because
# naming varies: seL4 uses LICENSE.md, musllibc uses COPYRIGHT, OpenSBI uses
# COPYING.BSD. The authoritative texts are the LICENSES/ directories inside
# each tree; this only establishes that a tree is intact.
LICENSE_PREFIXES = ("licen", "copying", "copyright")

CHUNK = 1 << 20
NETWORK_TIMEOUT = 60
DOWNLOAD_ATTEMPTS = 3


class PinError(RuntimeError):
    """A pinned input is missing, unverified or in an unexpected state."""


def fetch(url: str, sha256: str, filename: str, *, user_agent: str = "aegir2") -> Path:
    """Return a verified local copy of `url`, downloading it if not cached.

    The cache is third_party/download/, which is gitignored. A cached file that
    does not match the pin is discarded rather than trusted.
    """
    DOWNLOAD.mkdir(parents=True, exist_ok=True)
    archive = DOWNLOAD / filename
    if archive.is_file() and sha256_file(archive) == sha256:
        print(f"INFO  cached {archive.name}", flush=True)
        return archive
    if archive.is_file():
        print(f"INFO  discarding {archive.name}: does not match the pin", flush=True)
        archive.unlink()

    last_error: Exception | None = None
    for attempt in range(1, DOWNLOAD_ATTEMPTS + 1):
        try:
            print(f"INFO  downloading {url} (attempt {attempt})", flush=True)
            request = urllib.request.Request(url, headers={"User-Agent": user_agent})
            with urllib.request.urlopen(request, timeout=NETWORK_TIMEOUT) as response:
                with archive.open("wb") as handle:
                    shutil.copyfileobj(response, handle, CHUNK)
            last_error = None
            break
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            last_error = exc
            print(f"INFO  download failed: {exc}", flush=True)
    if last_error is not None:
        raise PinError(f"download failed after {DOWNLOAD_ATTEMPTS} attempts: {last_error}")

    actual = sha256_file(archive)
    if actual != sha256:
        archive.unlink()
        raise PinError(f"sha256 mismatch for {filename}: expected {sha256}, got {actual}")
    return archive


def extract(archive: Path, target_root: Path) -> Path:
    """Extract an archive under `target_root`, keeping its top-level directory.

    Vendor tarballs rely on their internal layout, so nothing is renamed or
    relocated; the single top-level directory that appeared is returned.
    """
    target_root.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as tar:
        names = [member.name for member in tar.getmembers() if member.name]
        roots = {name.split("/", 1)[0] for name in names}
        if len(roots) != 1:
            raise PinError(
                f"expected one top-level directory in {archive.name}, found {sorted(roots)}"
            )
        root = roots.pop()
        target = target_root / root
        if target.exists():
            shutil.rmtree(target)
        tar.extractall(path=target_root, filter="data")
    return target


AR_MAGIC = b"!<arch>\n"
AR_HEADER_SIZE = 60


def deb_payload(archive: Path) -> bytes:
    """Return the `data.tar.*` member of a Debian package.

    A .deb is an `ar` archive containing `debian-binary`, `control.tar.*` and
    `data.tar.*`. The format is simple enough to read here, which keeps the
    extraction free of external tools (`ar`, `dpkg-deb`) that a host may lack.
    """
    blob = archive.read_bytes()
    if not blob.startswith(AR_MAGIC):
        raise PinError(f"{archive.name} is not an ar archive, so not a .deb")
    offset = len(AR_MAGIC)
    while offset + AR_HEADER_SIZE <= len(blob):
        header = blob[offset : offset + AR_HEADER_SIZE]
        offset += AR_HEADER_SIZE
        name = header[:16].decode("ascii", "replace").strip()
        try:
            size = int(header[48:58].decode("ascii").strip())
        except ValueError as exc:
            raise PinError(f"{archive.name}: unreadable ar header for {name!r}") from exc
        member = blob[offset : offset + size]
        offset += size + (size % 2)
        if name.startswith("data.tar."):
            return member
    raise PinError(f"{archive.name}: no data.tar.* member")


def extract_deb(archive: Path, target: Path) -> None:
    """Extract a .deb's payload into `target`.

    Multiple packages are extracted into the same tree on purpose: Debian ships
    the compiler and its runtime libraries (libisl, libgmp, libmpfr, libmpc)
    separately, and they merge into one usable prefix.
    """
    target.mkdir(parents=True, exist_ok=True)
    payload = io.BytesIO(deb_payload(archive))
    with tarfile.open(fileobj=payload, mode="r:*") as tar:
        tar.extractall(path=target, filter="data")


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


def license_files(repository: Path) -> list[str]:
    """Root-level license files of a vendored tree (see LICENSE_PREFIXES)."""
    if not repository.is_dir():
        return []
    return sorted(
        entry.name
        for entry in repository.iterdir()
        if entry.name.lower().startswith(LICENSE_PREFIXES)
    )


def git(repository: Path, *arguments: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    """Run git in a repository and return the completed process."""
    return subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=check,
        capture_output=True,
        text=True,
        timeout=120,
    )
