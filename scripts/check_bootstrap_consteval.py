#!/usr/bin/env python3
"""Exercise bootstrap Meta arithmetic through generated LAINIR #eval."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / "scripts" / "run_lain_compiler.py"
FIXTURES = (
    ROOT / "scripts" / "fixtures" / "formal_consteval_arithmetic.lain",
    ROOT / "scripts" / "fixtures" / "formal_meta_scalar_call.lain",
)
SEED = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)


def run(arguments: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(arguments, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-bootstrap-consteval-") as directory:
        for fixture in FIXTURES:
            artifact = Path(directory) / f"{fixture.stem}.l1"
            compiled = run(
                [
                    sys.executable,
                    str(COMPILER),
                    "--library",
                    "-o",
                    str(artifact),
                    str(fixture),
                ]
            )
            if compiled.returncode:
                print(compiled.stderr or compiled.stdout, file=sys.stderr)
                return compiled.returncode

            text = artifact.read_text(encoding="utf-8")
            if "#eval" in text:
                print(
                    f"bootstrap Meta: {fixture.name} retained #eval",
                    file=sys.stderr,
                )
                return 1

            executed = run([str(SEED), "run", str(artifact), "main"])
            if executed.returncode or executed.stdout.strip() != "42":
                print(executed.stderr or executed.stdout, file=sys.stderr)
                return executed.returncode or 1

    print("PASS bootstrap consteval arithmetic and scalar calls through LAINVM")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
