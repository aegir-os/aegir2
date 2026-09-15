"""Shared helpers for Aegir's host tooling.

Knows where pinned third-party artifacts live, how to hash a file, and how a
fetch/install records itself in a stamp file. Stdlib only, so these scripts run
on a bare host with nothing but python3.
"""

from __future__ import annotations

import hashlib
import json
import tomllib
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
