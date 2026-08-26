#!/usr/bin/env python3
"""Run the first Lain-written canonical-L1 -> C backend kernel.

The generated backend L1 is bootstrapped by the frozen compiler.  Its only
host dependencies are the source-data and artifact byte-stream capabilities
already provided by ``lainir-seed``.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)
BACKEND = ROOT / "src" / "lainc" / "backend_c.lain"
COMPILE = ROOT / "scripts" / "run_lain_compiler.py"


def run(*args: Path | str) -> None:
    result = subprocess.run(
        [str(arg) for arg in args],
        cwd=ROOT,
        text=True,
    )
    if result.returncode:
        raise SystemExit(result.returncode)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="canonical L1 input")
    parser.add_argument("-o", "--output", type=Path, required=True, help="C output")
    parser.add_argument(
        "--backend-l1",
        type=Path,
        default=ROOT / "build" / "backend_c_entry.l1",
        help="cached generated backend L1",
    )
    args = parser.parse_args()

    run(sys.executable, COMPILE, "-o", args.backend_l1, BACKEND)
    run(SEED, "interpreter", args.backend_l1, "main", args.output, args.input)
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
