#!/usr/bin/env python3
"""src/lainc consteval evaluation acceptance.

Chain: gen1 (frozen lainc) compiles src/lainc/lainc.lain; gen2 (the
Lain-written compiler) compiles consteval fixtures.  Verifies the
compile-time evaluator handles:
  - nested arithmetic expressions (x * y + 1)
  - nested expressions in call arguments (calc(2 + 3, 4 * 2))
  - local let bindings in consteval bodies
  - references to previously bound top-level constants
  - if/else branches with comparisons
Each product must l1check clean and run to the expected value.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "seed" / "zig-out" / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""
SEED = BIN / f"lainir-seed{SUFFIX}"
PRINT = BIN / f"lainir-print{SUFFIX}"
FROZEN = ROOT / "src" / "lainir" / "lainc.l1"
LAINC = ROOT / "src" / "lainc" / "lainc.lain"
FIX = Path(__file__).parent / "fixtures"


def run(*arguments: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(a) for a in arguments], cwd=ROOT, capture_output=True, text=True
    )


def main() -> int:
    if not SEED.exists() or not PRINT.exists():
        raise RuntimeError("build seed first: cd seed && zig build")
    with tempfile.TemporaryDirectory(prefix="lainc-ce-") as directory:
        tmp = Path(directory)
        gen1 = tmp / "gen1.l1"
        gen2 = tmp / "gen2.l1"
        empty = tmp / "empty.lain"
        empty.write_text("", encoding="utf-8")

        compiled = run(SEED, FROZEN, "compiler_compile", gen1, LAINC)
        if compiled.returncode:
            raise RuntimeError(compiled.stderr or compiled.stdout)
        gen2_run = run(SEED, "interpreter", gen1, "compiler_compile", gen2, LAINC)
        if gen2_run.returncode:
            raise RuntimeError(gen2_run.stderr or gen2_run.stdout)

        def compile_fixture(name: str) -> Path:
            out = tmp / f"{name}.l1"
            res = run(
                SEED, "interpreter", gen2, "compiler_compile", out, FIX / f"{name}.lain"
            )
            if res.returncode:
                raise RuntimeError(f"{name}: {res.stderr or res.stdout}")
            checked = run(PRINT, out, "main")
            if checked.returncode:
                raise RuntimeError(f"{name} l1check: {checked.stderr}")
            return out

        def run_main(product: Path) -> str:
            ran = run(SEED, "run", product, "main")
            if ran.returncode:
                raise RuntimeError(f"run {product.name}: {ran.stderr or ran.stdout}")
            return ran.stdout.strip()

        cases = {
            "consteval_nested": "43",      # 6 * 7 + 1
            "consteval_arg_expr": "41",    # (2+3) * (4*2) + 1
            "consteval_locals": "42",      # base = x*10; bonus = 2; base+bonus
            "consteval_const_ref": "84",   # A = add1(41); double(A) = A*2
            "consteval_if": "1",           # classify(40) == 40 -> 1
        }
        for name, expected in cases.items():
            out = compile_fixture(name)
            actual = run_main(out)
            if actual != expected:
                raise RuntimeError(f"{name}: ran {actual}, expected {expected}")
            print(f"PASS {name} -> {actual}")
    print("PASS src/lainc consteval evaluation")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
