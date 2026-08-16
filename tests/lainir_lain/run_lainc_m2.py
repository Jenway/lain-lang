#!/usr/bin/env python3
"""M2 acceptance: the Lain-written lainc bootstraps to a fixed point.

Chain:
  gen1 = frozen lainc (src/lainir/lainc.l1) compiles src/lainc/lainc.lain
  gen2 = gen1 (compiler_compile) compiles src/lainc/lainc.lain
  gen3 = gen2 (compiler_compile) compiles src/lainc/lainc.lain

Acceptance: gen2 and gen3 are byte-identical (the self-hosting fixed point)
and gen2 passes lainir-print (l1check).
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


def run(*arguments: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(a) for a in arguments], cwd=ROOT, capture_output=True, text=True
    )


def main() -> int:
    if not SEED.exists() or not PRINT.exists():
        raise RuntimeError("build seed first: cd seed && zig build")
    with tempfile.TemporaryDirectory(prefix="lainc-m2-") as directory:
        tmp = Path(directory)
        gen1 = tmp / "gen1.l1"
        gen2 = tmp / "gen2.l1"
        gen3 = tmp / "gen3.l1"
        empty = tmp / "empty.lain"
        empty.write_text("", encoding="utf-8")

        # gen1: frozen lainc compiles the Lain-written lainc.
        compiled = run(SEED, FROZEN, "compiler_compile", gen1, LAINC)
        if compiled.returncode:
            raise RuntimeError(compiled.stderr or compiled.stdout)
        checked = run(PRINT, gen1, "compiler_compile")
        if checked.returncode:
            raise RuntimeError(f"gen1 l1check: {checked.stderr}")

        # gen2: the Lain-written lainc compiles itself.
        gen2_run = run(SEED, "interpreter", gen1, "compiler_compile", gen2, LAINC)
        if gen2_run.returncode:
            raise RuntimeError(gen2_run.stderr or gen2_run.stdout)
        checked2 = run(PRINT, gen2, "compiler_compile")
        if checked2.returncode:
            raise RuntimeError(f"gen2 l1check: {checked2.stderr}")

        # gen3: gen2 (as a compiler) compiles itself again.
        gen3_run = run(SEED, "interpreter", gen2, "compiler_compile", gen3, LAINC)
        if gen3_run.returncode:
            raise RuntimeError(gen3_run.stderr or gen3_run.stdout)
        checked3 = run(PRINT, gen3, "compiler_compile")
        if checked3.returncode:
            raise RuntimeError(f"gen3 l1check: {checked3.stderr}")

        text2 = gen2.read_bytes()
        text3 = gen3.read_bytes()
        if text2 != text3:
            raise RuntimeError(
                f"self-hosting did not converge: gen2 {len(text2)} bytes, "
                f"gen3 {len(text3)} bytes"
            )
    print(
        f"PASS Lain-written lainc fixed point: gen2 == gen3 "
        f"({len(text2)} bytes), both l1check clean"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
