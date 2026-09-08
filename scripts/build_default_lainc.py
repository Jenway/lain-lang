#!/usr/bin/env python3
"""Build native lainc from the checked-in bootstrap snapshot."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SNAPSHOT = ROOT / "bootstrap" / "lainc.l1"
CHECK_SNAPSHOT = ROOT / "scripts" / "check_lainc_bootstrap_snapshot.py"
BUILD_NATIVE = ROOT / "scripts" / "build_lainc_native.py"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "lainc.exe")
    parser.add_argument(
        "--c-output", type=Path, default=ROOT / "build" / "lainc-native.c"
    )
    args = parser.parse_args()
    output = args.output if args.output.is_absolute() else ROOT / args.output
    c_output = args.c_output if args.c_output.is_absolute() else ROOT / args.c_output
    checked = subprocess.run(
        [sys.executable, str(CHECK_SNAPSHOT), str(SNAPSHOT)], cwd=ROOT, text=True
    )
    if checked.returncode:
        return checked.returncode
    built = subprocess.run(
        [
            sys.executable,
            str(BUILD_NATIVE),
            str(SNAPSHOT),
            str(output),
            str(c_output),
        ],
        cwd=ROOT,
        text=True,
    )
    return built.returncode


if __name__ == "__main__":
    raise SystemExit(main())
