#!/usr/bin/env python3
"""Run a supplied native lainc over a CRLF user source and verify its output."""

from __future__ import annotations

import argparse
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BIN = ROOT / "seed" / "zig-out" / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""
CHECK = BIN / f"lainir-print{SUFFIX}"
SEED = BIN / f"lainir-seed{SUFFIX}"


def run(*args: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=Path)
    args = parser.parse_args()
    if not args.compiler.is_file():
        raise RuntimeError(f"native compiler not found: {args.compiler}")
    with tempfile.TemporaryDirectory(prefix="lain-native-smoke-") as directory:
        temp = Path(directory)
        source = temp / "main.lain"
        artifact = temp / "main.l1"
        source.write_bytes(
            b"let main = std::func() -> i32 {\r\n"
            b"    return 40 + 2;\r\n"
            b"};\r\n"
        )
        generated = run(args.compiler, "-o", artifact, source)
        if generated.returncode:
            raise RuntimeError(generated.stderr or generated.stdout)
        checked = run(CHECK, artifact, "main")
        if checked.returncode:
            raise RuntimeError(checked.stderr or checked.stdout)
        executed = run(SEED, "run", artifact, "main")
        if executed.returncode or executed.stdout.strip() != "42":
            raise RuntimeError(executed.stderr or executed.stdout)
    print("native lainc CRLF compile/run smoke: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error)
        raise SystemExit(1)
