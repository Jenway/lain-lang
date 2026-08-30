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

from backend_manifest import write_manifest


ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)
BACKEND = ROOT / "src" / "lainc" / "backend_c.lain"
COMPILE = ROOT / "scripts" / "run_lain_compiler.py"
CHECK = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-print.exe" if os.name == "nt" else "lainir-print"
)


def run(*args: Path | str) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [str(arg) for arg in args],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )
    if result.returncode:
        print(result.stderr or result.stdout, file=sys.stderr)
        raise SystemExit(result.returncode)
    return result


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
    parser.add_argument(
        "--manifest",
        type=Path,
        help="link manifest path (defaults to <output>.manifest.json)",
    )
    args = parser.parse_args()

    # Backend output is never allowed to bypass the canonical L1 verifier.
    run(CHECK, args.input)
    manifest = args.manifest or args.output.with_suffix(args.output.suffix + ".manifest.json")
    write_manifest(args.input, manifest)
    run(sys.executable, COMPILE, "-o", args.backend_l1, BACKEND)
    run(SEED, "interpreter", args.backend_l1, "main", args.output, args.input)
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
