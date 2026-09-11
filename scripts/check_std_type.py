#!/usr/bin/env python3
"""Verify the standard Meta type universe and rejection of its old spelling."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / "scripts" / "run_lain_compiler.py"
SEED = seed_exe("lainir-seed")
POSITIVE = ROOT / "scripts" / "fixtures" / "formal_std_type_value.lain"
NEGATIVE = ROOT / "scripts" / "fixtures" / "formal_bare_type_rejected.lain"


def run(arguments: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(arguments, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-std-type-") as directory:
        artifact = Path(directory) / "std_type.l1"
        compiled = run(
            [
                sys.executable,
                str(COMPILER),
                "--library",
                "-o",
                str(artifact),
                str(POSITIVE),
            ]
        )
        if compiled.returncode:
            print(compiled.stderr or compiled.stdout, file=sys.stderr)
            return compiled.returncode
        if "#eval" in artifact.read_text(encoding="utf-8"):
            print("std::type fixture retained #eval", file=sys.stderr)
            return 1
        executed = run([str(SEED), "run", str(artifact), "main"])
        if executed.returncode or executed.stdout.strip() != "42":
            print(executed.stderr or executed.stdout, file=sys.stderr)
            return executed.returncode or 1

        rejected = run(
            [
                sys.executable,
                str(COMPILER),
                "--library",
                "-o",
                str(Path(directory) / "bare_type.l1"),
                str(NEGATIVE),
            ]
        )
        if rejected.returncode == 0:
            print("bare type was accepted", file=sys.stderr)
            return 1

    print("PASS std::type binding and bare type rejection")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
