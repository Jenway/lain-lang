#!/usr/bin/env python3
"""Run the LAIN-IR-written Lain frontend against one or more source files."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "scripts" / "build_lain_frontend.py"
L1BOOTSTRAP = ROOT / "bootstrap" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)
BUNDLE = ROOT / "build" / "lainir" / "lain_frontend.l1"


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
    parser.add_argument("sources", nargs="+", type=Path)
    args = parser.parse_args()
    built = run([sys.executable, BUILD])
    if built.returncode:
        print(built.stderr or built.stdout, file=sys.stderr)
        return built.returncode
    executed = run(
        [
            L1BOOTSTRAP,
            BUNDLE,
            "lain_compile_dump",
            args.output,
            *(ROOT / source for source in args.sources),
        ]
    )
    if executed.returncode:
        print(executed.stderr or executed.stdout, file=sys.stderr)
        return executed.returncode
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
