#!/usr/bin/env python3
"""Run the first Lain-written canonical-L1 -> C backend kernel.

The generated backend L1 is bootstrapped by the frozen compiler. Its host
dependencies are tracked by the backend capability ABI; the logical
declaration inventory is checked before compilation.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from backend_manifest import write_manifest
from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
SEED = seed_exe("lainir-seed")
BACKEND = ROOT / "src" / "lainc" / "backend_c.lain"
ABI_CHECK = ROOT / "scripts" / "check_lain_backend_abi.py"
COMPILE = ROOT / "scripts" / "run_lain_compiler.py"
CHECK = seed_exe("lainir-print")


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
    # Fail at the source boundary with the actionable ABI inventory instead
    # of exposing the compiler's generic legacy-attribute diagnostic.
    run(sys.executable, ABI_CHECK)
    manifest = args.manifest or args.output.with_suffix(args.output.suffix + ".manifest.json")
    # The backend is a library artifact with an explicit `main` entry used by
    # the seed driver.  `--library` selects the source-compiler path without
    # applying the user-program main-return policy (which rejects the backend
    # driver's i32 entry as status 5112).
    run(sys.executable, COMPILE, "--library", "-o", args.backend_l1, BACKEND)
    # The capability manifest describes the backend artifact that is about to
    # run.  The input artifact may have its own user externs and must not be
    # mistaken for the backend provider contract.
    write_manifest(args.backend_l1, manifest)
    run(SEED, "interpreter", args.backend_l1, "main", args.output, args.input)
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
