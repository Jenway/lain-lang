#!/usr/bin/env python3
"""Build the reproducible LAIN-IR Lain frontend artifact."""

from __future__ import annotations

import subprocess
import sys
import os
import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
L1CHECK = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-print.exe" if os.name == "nt" else "lainir-print"
)
OUTPUT = ROOT / "build" / "lainir" / "lain_frontend.l1"
STAMP = OUTPUT.with_suffix(".stamp.json")
SCHEMA = "lain-frontend-bundle-v1"
MODULES = (
    ROOT / "src" / "lainir" / "tools" / "source.l1",
    ROOT / "src" / "lainir" / "lain" / "raw_ast.l1",
    ROOT / "src" / "lainir" / "lain" / "meta.l1",
    ROOT / "src" / "lainir" / "lain" / "module_meta.l1",
    ROOT / "src" / "lainir" / "lain" / "eval.l1",
    ROOT / "src" / "lainir" / "lain" / "workspace.l1",
    ROOT / "src" / "lainir" / "lain" / "workspace_cache.l1",
    ROOT / "src" / "lainir" / "lain" / "driver.l1",
)


def run(arguments: list[Path | str]) -> None:
    result = subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def input_fingerprint() -> str:
    digest = hashlib.sha256(SCHEMA.encode("utf-8"))
    digest.update(BUNDLER.read_bytes())
    for path in MODULES:
        digest.update(str(path.relative_to(ROOT)).replace("\\", "/").encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def cache_valid(fingerprint: str) -> bool:
    if not OUTPUT.is_file() or not STAMP.is_file():
        return False
    try:
        stamp = json.loads(STAMP.read_text(encoding="utf-8"))
        output_hash = hashlib.sha256(OUTPUT.read_bytes()).hexdigest()
    except (OSError, ValueError, TypeError):
        return False
    return (
        stamp.get("schema") == SCHEMA
        and stamp.get("inputs") == fingerprint
        and stamp.get("output") == output_hash
    )


def write_stamp(fingerprint: str) -> None:
    STAMP.write_text(
        json.dumps(
            {
                "schema": SCHEMA,
                "inputs": fingerprint,
                "output": hashlib.sha256(OUTPUT.read_bytes()).hexdigest(),
            },
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )


def main() -> int:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    fingerprint = input_fingerprint()
    if cache_valid(fingerprint):
        try:
            run([L1CHECK, OUTPUT, "lain_compile_dump"])
        except RuntimeError:
            pass
        else:
            print(f"{OUTPUT.relative_to(ROOT)} (cache hit)")
            return 0
    run([sys.executable, BUNDLER, "-o", OUTPUT, *MODULES])
    run([L1CHECK, OUTPUT, "lain_compile_dump"])
    write_stamp(fingerprint)
    print(OUTPUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"lain frontend build failed: {error}", file=sys.stderr)
        raise SystemExit(1)
