#!/usr/bin/env python3
"""Verify that a native lainc can execute its emitted L1 in-process."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BIN = ROOT / "seed" / "zig-out" / "bin"
SUFFIX = ".exe" if __import__("os").name == "nt" else ""
CHECK = BIN / f"lainir-print{SUFFIX}"


def run(*args: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=Path)
    args = parser.parse_args()
    if not args.compiler.is_file():
        raise RuntimeError(f"native compiler not found: {args.compiler}")
    with tempfile.TemporaryDirectory(prefix="lain-native-inproc-") as directory:
        temp = Path(directory)
        source = temp / "main.lain"
        artifact = temp / "main.l1"
        source.write_text(
            "let main = std::func() -> i32 {\n    return 40 + 2;\n};\n",
            encoding="utf-8",
            newline="\n",
        )
        executed = run(args.compiler, "--run", "-o", artifact, source)
        if executed.returncode or executed.stdout.strip() != "42":
            raise RuntimeError(executed.stderr or executed.stdout)
        checked = run(CHECK, artifact, "main")
        if checked.returncode:
            raise RuntimeError(checked.stderr or checked.stdout)
    print("native lainc in-process L1 execution smoke: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error)
        raise SystemExit(1)
