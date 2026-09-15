#!/usr/bin/env python3
"""Fetch and verify Aegir's pinned RISC-V cross toolchain.

Downloads the artifacts named in manifests/toolchain.toml into an ignored
cache, verifies every sha256 against the pin, and unpacks them into a single
prefix under `third_party/toolchain/`. Nothing is installed outside the
repository and no root is required.

    python3 scripts/fetch_toolchain.py            # fetch/extract if needed
    python3 scripts/fetch_toolchain.py --check    # verify only, no network

`--check` is not a formality: it compiles probes to prove the toolchain builds
for our ABI *and* that it uses native TLS. A toolchain configured with
--disable-tls (xPack's riscv-none-elf is) emits emulated TLS, which seL4's
userland cannot run on — and that failure shows up much later, as a crash at
address 0 in the root task, so it is worth catching here.

Exit status: 0 success, 1 pin/verification failure, 2 usage error.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pins

COMMAND_TIMEOUT = 300
# Debian's multiarch library directory, where the extracted .deb payloads put
# libisl/libgmp/libmpfr/libmpc.
LIBRARY_SUBDIR = Path("usr/lib/x86_64-linux-gnu")
SMOKE_SOURCE = "int aegir_toolchain_smoke(void) { return 0; }\n"
TLS_SOURCE = "__thread int aegir_tls_probe;\nint aegir_tls_read(void) { return aegir_tls_probe; }\n"
TLS_SYMBOL = "aegir_tls_probe"


def toolchain_env(prefix: Path) -> dict[str, str]:
    """Environment for running the toolchain.

    Debian's cc1 links libisl/libgmp/libmpfr/libmpc shared, and a host may not
    have all of them (a Fedora host has no libisl.so.23 at all), so the copies
    extracted alongside the toolchain are put on the search path. scripts/env.sh
    does the same for builds.
    """
    environment = os.environ.copy()
    library_dir = prefix / LIBRARY_SUBDIR
    if library_dir.is_dir():
        existing = environment.get("LD_LIBRARY_PATH", "")
        environment["LD_LIBRARY_PATH"] = (
            f"{library_dir}:{existing}" if existing else str(library_dir)
        )
    return environment


def run_capture(command: list[str], prefix: Path, stdin_text: str | None = None) -> str:
    """Run a toolchain command and return its stripped stdout, or raise."""
    completed = subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
        timeout=COMMAND_TIMEOUT,
        env=toolchain_env(prefix),
        input=stdin_text,
    )
    return completed.stdout.strip()


def install_prefix(pin: dict[str, object]) -> Path:
    return pins.TOOLCHAIN_ROOT / f"{pin['name']}-{pin['version']}"


def abi_flags(pin: dict[str, object]) -> list[str]:
    return [f"-march={pin['require_march']}", f"-mabi={pin['require_mabi']}"]


def check_abi(gcc: Path, pin: dict[str, object], prefix: Path) -> str:
    """Prove the toolchain builds for our exact ABI.

    Asks the compiler for its predefined macros: that is the ABI it will
    actually produce, independent of how it names multilib directories. For our
    flags Debian's default multilib is the right one, so a "." from
    -print-multi-directory is expected rather than an error.
    """
    macros = run_capture([str(gcc), *abi_flags(pin), "-dM", "-E", "-"], prefix, stdin_text="")
    float_abi = str(pin["require_float_abi"])
    word_size = str(pin["require_word_size"])
    if float_abi not in macros:
        raise pins.PinError(
            f"{pin['require_march']}/{pin['require_mabi']} does not select {float_abi}"
        )
    if word_size not in macros:
        raise pins.PinError(f"{word_size} not defined by {gcc.name}")
    directory = run_capture([str(gcc), "-print-multi-directory", *abi_flags(pin)], prefix) or "."
    libgcc = Path(run_capture([str(gcc), *abi_flags(pin), "-print-libgcc-file-name"], prefix))
    if not libgcc.is_file():
        raise pins.PinError(f"libgcc is missing at {libgcc}")
    return f"{directory} ({libgcc.name})"


def check_compiles(gcc: Path, pin: dict[str, object], prefix: Path) -> None:
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
                "-ffreestanding",
                "-nostdlib",
                "-c",
                "-o",
                str(workdir / "smoke.o"),
                str(source),
            ],
            prefix,
        )
        if not (workdir / "smoke.o").is_file():
            raise pins.PinError("toolchain compiled nothing")


def check_native_tls(gcc: Path, pin: dict[str, object], prefix: Path) -> None:
    """Fail if the toolchain emits emulated TLS (see the module docstring)."""
    with tempfile.TemporaryDirectory(prefix="aegir-toolchain-") as tmp:
        workdir = Path(tmp)
        source = workdir / "tls.c"
        source.write_text(TLS_SOURCE, encoding="utf-8")
        obj = workdir / "tls.o"
        run_capture(
            [
                str(gcc),
                *abi_flags(pin),
                "-ffreestanding",
                "-ftls-model=local-exec",
                "-c",
                "-o",
                str(obj),
                str(source),
            ],
            prefix,
        )
        nm = gcc.with_name(f"{pin['prefix']}nm")
        symbols = run_capture([str(nm), str(obj)], prefix)
        if "__emutls" in symbols:
            raise pins.PinError(
                "emulated TLS detected (__emutls_*): seL4's userland requires native "
                "tp-relative TLS, so any root task built with this compiler crashes at "
                "startup. Use a toolchain configured with --enable-tls."
            )
        if TLS_SYMBOL not in symbols:
            raise pins.PinError(f"TLS probe lost its symbol ({TLS_SYMBOL})")


def check() -> int:
    pin = dict(pins.load_pins()["toolchain"])
    name = str(pin["name"])
    stamp = pins.read_stamp(name)
    if stamp is None:
        pins.report(False, f"{name} is not installed", "run: make tools")
        return 1
    pinned_hashes = sorted(str(artifact["sha256"]) for artifact in pin["artifacts"])
    if stamp.get("sha256") != pinned_hashes or stamp.get("version") != pin["version"]:
        pins.report(
            False,
            f"{name} stamp does not match the pin",
            f"pin {pin['version']} / {len(pinned_hashes)} artifacts",
        )
        return 1

    prefix = pins.ROOT / str(stamp.get("path", ""))
    gcc = prefix / "usr" / "bin" / f"{pin['prefix']}gcc"
    gxx = prefix / "usr" / "bin" / f"{pin['prefix']}g++"
    for tool in (gcc, gxx):
        if not tool.is_file():
            pins.report(False, f"missing {tool.relative_to(pins.ROOT)}")
            return 1
    if not (prefix / LIBRARY_SUBDIR).is_dir():
        pins.report(
            False,
            "toolchain runtime libraries are missing",
            f"expected {LIBRARY_SUBDIR} under {prefix.relative_to(pins.ROOT)}",
        )
        return 1

    try:
        abi = check_abi(gcc, pin, prefix)
        check_compiles(gcc, pin, prefix)
        check_native_tls(gcc, pin, prefix)
    except (pins.PinError, OSError, subprocess.SubprocessError) as exc:
        pins.report(False, f"{name} cannot build for our target", str(exc))
        return 1

    pins.report(
        True,
        f"{name} {pin['version']} present",
        f"{pin['require_march']}/{pin['require_mabi']} -> {abi}, native TLS",
    )
    return 0


def fetch_artifacts(pin: dict[str, object], prefix: Path) -> None:
    """Fetch, verify and unpack every pinned artifact into `prefix`."""
    if prefix.exists():
        shutil.rmtree(prefix)
    for artifact in pin["artifacts"]:
        archive = pins.fetch(
            str(artifact["url"]), str(artifact["sha256"]), str(artifact["archive"])
        )
        pins.extract_deb(archive, prefix)


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
        name = str(pin["name"])
        stamp = pins.read_stamp(name)
        already_pinned = sorted(str(a["sha256"]) for a in pin["artifacts"])
        if (
            stamp is not None
            and stamp.get("version") == pin["version"]
            and stamp.get("sha256") == already_pinned
            and check() == 0
        ):
            return 0

        prefix = install_prefix(pin)
        fetch_artifacts(pin, prefix)
        if not (prefix / LIBRARY_SUBDIR).is_dir():
            raise pins.PinError(f"no {LIBRARY_SUBDIR} in the extracted toolchain")
        pins.write_stamp(
            name,
            {
                "name": pin["name"],
                "version": pin["version"],
                "sha256": already_pinned,
                "urls": [str(a["url"]) for a in pin["artifacts"]],
                "prefix": pin["prefix"],
                "path": str(prefix.relative_to(pins.ROOT)),
            },
        )
        print(f"INFO  installed {prefix.relative_to(pins.ROOT)}", flush=True)
    except (pins.PinError, OSError, subprocess.SubprocessError) as exc:
        pins.report(False, "toolchain fetch failed", str(exc))
        return 1
    return check()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
