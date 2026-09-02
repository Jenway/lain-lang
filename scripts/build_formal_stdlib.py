#!/usr/bin/env python3
"""Build and verify the formal Lain standard-library source closure.

The result is deliberately separate from ``bootstrap_std.l1``.  The latter
is the LAIN-IR bootstrap implementation used by the compiler; this artifact
is the output of compiling the user-facing ``std/**/*.lain`` sources.
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUN_COMPILER = ROOT / "scripts" / "run_lain_compiler.py"
SEED_PRINT = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-print.exe" if os.name == "nt" else "lainir-print"
)
OUTPUT = ROOT / "build" / "lainir" / "formal_stdlib.l1"
MANIFEST = ROOT / "build" / "lainir" / "formal_stdlib.manifest.json"


def run(arguments: list[Path | str]) -> None:
    result = subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def sources() -> tuple[Path, ...]:
    return tuple(
        sorted(
            (path for path in (ROOT / "std").rglob("*.lain") if path.is_file()),
            key=lambda path: path.relative_to(ROOT).as_posix(),
        )
    )


def source_fingerprint(paths: tuple[Path, ...]) -> str:
    digest = hashlib.sha256(b"lain-formal-stdlib-source-closure-v1")
    for path in paths:
        digest.update(path.relative_to(ROOT).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def write_manifest(paths: tuple[Path, ...], fingerprint: str) -> None:
    MANIFEST.write_text(
        json.dumps(
            {
                "schema": "lain-formal-stdlib-source-closure-v1",
                "artifact": OUTPUT.relative_to(ROOT).as_posix(),
                "artifact_sha256": hashlib.sha256(OUTPUT.read_bytes()).hexdigest(),
                "inputs_sha256": fingerprint,
                "target_independent": True,
                "abi": "lain_std_abi_v1",
                "abi_status": "source-closure-only",
                "abi_entries": [],
                "sources": [
                    {
                        "path": path.relative_to(ROOT).as_posix(),
                        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    }
                    for path in paths
                ],
            },
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )


def main() -> int:
    paths = sources()
    if not paths:
        raise RuntimeError("no formal standard-library sources found")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    run(
        [
            sys.executable,
            RUN_COMPILER,
            "--library",
            "-o",
            OUTPUT,
            *paths,
        ]
    )
    run([SEED_PRINT, OUTPUT])
    fingerprint = source_fingerprint(paths)
    write_manifest(paths, fingerprint)
    print(f"{OUTPUT.relative_to(ROOT)}")
    print(f"{MANIFEST.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"formal stdlib build failed: {error}", file=sys.stderr)
        raise SystemExit(1)
