#!/usr/bin/env python3
"""Compile the first Lain source subset to executable LAIN-IR text."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "scripts" / "build_lain_compiler.py"
L1BOOTSTRAP = ROOT / "bootstrap" / "zig-out" / "bin" / (
    "l1bootstrap.exe" if os.name == "nt" else "l1bootstrap"
)
BUNDLE = ROOT / "build" / "lainir" / "lain_compiler.l1"


def run(arguments: list[Path | str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output", required=True, type=Path)
    parser.add_argument(
        "--library",
        action="store_true",
        help="compile a source closure without requiring a native main entry",
    )
    parser.add_argument("source", type=Path, nargs="+")
    args = parser.parse_args()
    built = run([sys.executable, BUILD])
    if built.returncode:
        print(built.stderr or built.stdout, file=sys.stderr)
        return built.returncode
    executed = run(
        [
            L1BOOTSTRAP,
            BUNDLE,
            "compiler_compile_library" if args.library else "compiler_compile",
            args.output,
            *(ROOT / source for source in args.source),
        ]
    )
    if executed.returncode:
        print(executed.stderr or executed.stdout, file=sys.stderr)
        return executed.returncode
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
