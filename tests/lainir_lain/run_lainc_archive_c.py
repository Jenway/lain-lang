#!/usr/bin/env python3
"""src/lainc stage-C acceptance: module factory evaluation.

Verifies with gen2:
  - `let lex: Module = t.make(1);` evaluates the factory at compile time:
    the factory body's std::func members are emitted as f0_<factory>_<m>
    and bound under a factory-produced module row
  - calls through the produced module value resolve to those labels
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
    with tempfile.TemporaryDirectory(prefix="lainc-fact-") as directory:
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

        out = tmp / "factory.l1"
        res = run(SEED, "interpreter", gen2, "compiler_compile_library", out, FIX / "t.lain", FIX / "app_t.lain")
        if res.returncode:
            raise RuntimeError(f"factory: {res.stderr or res.stdout}")
        checked = run(PRINT, out, "main")
        if checked.returncode:
            raise RuntimeError(f"factory l1check: {checked.stderr}")
        text = out.read_text(encoding="utf-8")
        assert "#proc f0_make_five(" in text, "factory member not emitted"
        assert "#call f0_make_five()" in text, "lex.five() not resolved to factory label"
        ran = run(SEED, "run", out, "main")
        if ran.returncode:
            raise RuntimeError(f"factory run: {ran.stderr or ran.stdout}")
        assert ran.stdout.strip() == "5", f"factory run = {ran.stdout!r}"
        print("PASS module factory (t.make -> lex.five -> f0_make_five -> 5)")
    print("PASS src/lainc stage C: module factory evaluation")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
