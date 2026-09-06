#!/usr/bin/env python3
"""Build src/lainc with an explicitly selected LAINIR capability provider."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from lainc_sources import composed_compiler_sources


ROOT = Path(__file__).resolve().parents[1]
RUN_COMPILER = ROOT / "scripts" / "run_lain_compiler.py"
DEFAULT_OUTPUT = ROOT / "build" / "lainir" / "srclainc.l1"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    output = args.output if args.output.is_absolute() else ROOT / args.output
    output.parent.mkdir(parents=True, exist_ok=True)

    sources = composed_compiler_sources(ROOT)
    command = [
        sys.executable,
        str(RUN_COMPILER),
        "--library",
        "-o",
        str(output),
        *(str(path.relative_to(ROOT)) for path in sources),
    ]
    result = subprocess.run(command, cwd=ROOT, text=True)
    if result.returncode:
        return result.returncode
    print(output.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
