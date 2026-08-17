#!/usr/bin/env python3
"""src/lainc stage-A acceptance: multi-source library session + imports.

Chain: gen1 (frozen lainc) compiles src/lainc/lainc.lain; gen2 (the
Lain-written compiler) compiles a dependency-ordered source list via
compiler_compile_library.  Verifies:
  - import("packages::lain::compiler::X") binds the alias to the source
    whose basename is X
  - library sources get f0_<basename>_<name> labels (bare top-level fns)
  - module-bearing library sources keep f0_<module>_<member> labels
  - the entry source (last) keeps plain main
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
    with tempfile.TemporaryDirectory(prefix="lainc-lib-") as directory:
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

        def compile_library(name: str, *sources: str) -> Path:
            out = tmp / f"{name}.l1"
            res = run(
                SEED, "interpreter", gen2, "compiler_compile_library", out,
                *[FIX / s for s in sources],
            )
            if res.returncode:
                raise RuntimeError(f"{name}: {res.stderr or res.stdout}")
            checked = run(PRINT, out, "main")
            if checked.returncode:
                raise RuntimeError(f"{name} l1check: {checked.stderr}")
            return out

        # 1. bare library function: b.five -> f0_b_five, entry calls it.
        p1 = compile_library("case1", "b.lain", "app_a.lain")
        text = p1.read_text(encoding="utf-8")
        assert "#proc f0_b_five(" in text, "missing f0_b_five"
        assert "#call f0_b_five()" in text, "missing bee.five() call"
        ran = run(SEED, "run", p1, "main")
        if ran.returncode:
            raise RuntimeError(f"case1 run: {ran.stderr or ran.stdout}")
        assert ran.stdout.strip() == "5", f"case1 run = {ran.stdout!r}"
        print("PASS bare library fn (b.five -> f0_b_five -> 5)")

        # 2. module-bearing library: math.seven -> f0_math_seven, entry calls it.
        p2 = compile_library("case2", "b2.lain", "app_a2.lain")
        text = p2.read_text(encoding="utf-8")
        assert "#proc f0_math_seven(" in text, "missing f0_math_seven"
        assert "#call f0_math_seven()" in text, "missing math.seven() call"
        ran = run(SEED, "run", p2, "main")
        if ran.returncode:
            raise RuntimeError(f"case2 run: {ran.stderr or ran.stdout}")
        assert ran.stdout.strip() == "7", f"case2 run = {ran.stdout!r}"
        print("PASS module-bearing library (b2.math.seven -> f0_math_seven -> 7)")
    print("PASS src/lainc stage A: multi-source session + imports")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
