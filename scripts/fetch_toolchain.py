#!/usr/bin/env python3
"""Fetch and verify Aegir's pinned RISC-V cross toolchain.

Downloads the archive named in manifests/toolchain.toml into an ignored cache,
verifies its sha256 against the pin, and extracts it under
`third_party/toolchain/`. Nothing is installed outside the repository.

    python3 scripts/fetch_toolchain.py            # fetch/extract if needed
    python3 scripts/fetch_toolchain.py --check    # verify only, no network

Exit status: 0 success, 1 pin/verification failure, 2 usage error.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

import pins

NETWORK_TIMEOUT = 60
DOWNLOAD_ATTEMPTS = 3
COMMAND_TIMEOUT = 120
USER_AGENT = "aegir2-fetch-toolchain/1"

SMOKE_SOURCE = "int aegir_toolchain_smoke(void) { return 0; }\n"


def run_capture(command: list[str]) -> str:
    """Run a command and return its stripped stdout, or raise."""
    completed = subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
        timeout=COMMAND_TIMEOUT,
    )
    return completed.stdout.strip()


def abi_flags(pin: dict[str, object]) -> list[str]:
    return [f"-march={pin['require_march']}", f"-mabi={pin['require_mabi']}"]


def check_abi(gcc: Path, pin: dict[str, object]) -> str:
    """Return the multilib directory our arch/abi resolves to.

    Vendors name multilib directories differently, so the question is not "is
    there a directory called X" but "does this compiler resolve our flags to an
    installed multilib". We ask the compiler, then confirm the libgcc it would
    link against is really on disk.
    """
    flags = abi_flags(pin)
    directory = run_capture([str(gcc), "-print-multi-directory", *flags])
    if not directory or directory == ".":
        raise pins.PinError(
            f"{pin['require_march']}/{pin['require_mabi']} resolves to no specific "
            f"multilib (reported {directory!r})"
        )
    libgcc = Path(run_capture([str(gcc), *flags, "-print-libgcc-file-name"]))
    if not libgcc.is_file():
        raise pins.PinError(
            f"{pin['require_march']}/{pin['require_mabi']} resolves to {directory} "
            f"but libgcc is missing at {libgcc}"
        )
    return directory


def check_compiles(gcc: Path, pin: dict[str, object]) -> None:
    """Prove the toolchain can actually build for our target.

    A half-extracted or mismatched toolchain resolves multilibs happily and
    fails later inside a real build; compiling one freestanding object here
    keeps that failure close to its cause.
    """
    with tempfile.TemporaryDirectory(prefix="aegir-toolchain-") as tmp:
        workdir = Path(tmp)
        source = workdir / "smoke.c"
        source.write_text(SMOKE_SOURCE, encoding="utf-8")
        run_capture(
            [
                str(gcc),
                *abi_flags(pin),
                "-mcmodel=medany",
                "-ffreestanding",
                "-nostdlib",
                "-c",
                "-o",
                str(workdir / "smoke.o"),
                str(source),
            ]
        )
        if not (workdir / "smoke.o").is_file():
            raise pins.PinError("toolchain compiled nothing")


def check() -> int:
    pin = dict(pins.load_pins()["toolchain"])
    name = str(pin["name"])
    stamp = pins.read_stamp(name)
    if stamp is None:
        pins.report(False, f"{name} is not installed", "run: make tools")
        return 1
    if stamp.get("sha256") != pin["sha256"] or stamp.get("version") != pin["version"]:
        pins.report(
            False,
            f"{name} stamp does not match the pin",
            f"stamp {stamp.get('version')}/{str(stamp.get('sha256'))[:12]}, "
            f"pin {pin['version']}/{str(pin['sha256'])[:12]}",
        )
        return 1

    toolchain_dir = pins.ROOT / str(stamp.get("path", ""))
    gcc = toolchain_dir / "bin" / f"{pin['prefix']}gcc"
    gxx = toolchain_dir / "bin" / f"{pin['prefix']}g++"
    for tool in (gcc, gxx):
        if not tool.is_file():
            pins.report(False, f"missing {tool.relative_to(pins.ROOT)}")
            return 1

    try:
        directory = check_abi(gcc, pin)
        check_compiles(gcc, pin)
    except (pins.PinError, OSError, subprocess.SubprocessError) as exc:
        pins.report(False, f"{name} cannot build for our target", str(exc))
        return 1

    pins.report(
        True,
        f"{name} {pin['version']} present",
        f"{pin['require_march']}/{pin['require_mabi']} -> {directory}",
    )
    return 0


def download(pin: dict[str, object]) -> Path:
    """Return a local archive for the pin, downloading it if not cached."""
    pins.DOWNLOAD.mkdir(parents=True, exist_ok=True)
    archive = pins.DOWNLOAD / str(pin["archive"])
    expected = str(pin["sha256"])
    if archive.is_file() and pins.sha256_file(archive) == expected:
        print(f"INFO  cached {archive.name}", flush=True)
        return archive
    if archive.is_file():
        print(f"INFO  discarding {archive.name}: does not match the pin", flush=True)
        archive.unlink()

    last_error: Exception | None = None
    for attempt in range(1, DOWNLOAD_ATTEMPTS + 1):
        try:
            print(f"INFO  downloading {pin['url']} (attempt {attempt})", flush=True)
            request = urllib.request.Request(
                str(pin["url"]), headers={"User-Agent": USER_AGENT}
            )
            with urllib.request.urlopen(request, timeout=NETWORK_TIMEOUT) as response:
                with archive.open("wb") as handle:
                    shutil.copyfileobj(response, handle, pins.CHUNK)
            last_error = None
            break
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            last_error = exc
            print(f"INFO  download failed: {exc}", flush=True)
    if last_error is not None:
        raise pins.PinError(
            f"download failed after {DOWNLOAD_ATTEMPTS} attempts: {last_error}"
        )

    actual = pins.sha256_file(archive)
    if actual != expected:
        archive.unlink()
        raise pins.PinError(f"sha256 mismatch: expected {expected}, got {actual}")
    return archive


def extract(pin: dict[str, object], archive: Path) -> Path:
    """Extract the archive under third_party/toolchain/, verbatim.

    The archive's own top-level directory is kept: vendor tarballs rely on
    their internal layout, so nothing is renamed or relocated.
    """
    pins.TOOLCHAIN_ROOT.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as tar:
        members = tar.getmembers()
        roots = {member.name.split("/", 1)[0] for member in members if member.name}
        if len(roots) != 1:
            raise pins.PinError(
                f"expected one top-level directory in {archive.name}, found {sorted(roots)}"
            )
        root = roots.pop()
        target = pins.TOOLCHAIN_ROOT / root
        if target.exists():
            shutil.rmtree(target)
        tar.extractall(path=pins.TOOLCHAIN_ROOT, filter="data")
    return target


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the installed toolchain without fetching anything",
    )
    arguments = parser.parse_args(argv)

    if arguments.check:
        return check()

    try:
        pin = dict(pins.load_pins()["toolchain"])
        archive = download(pin)
        target = extract(pin, archive)
        pins.write_stamp(
            str(pin["name"]),
            {
                "name": pin["name"],
                "version": pin["version"],
                "sha256": pin["sha256"],
                "url": pin["url"],
                "prefix": pin["prefix"],
                "path": str(target.relative_to(pins.ROOT)),
            },
        )
        print(
            f"INFO  extracted to {target.relative_to(pins.ROOT)} "
            f"({pins.sha256_file(archive)[:12]}…)",
            flush=True,
        )
    except pins.PinError as exc:
        pins.report(False, "toolchain fetch failed", str(exc))
        return 1
    return check()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
