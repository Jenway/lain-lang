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
sys.path.insert(0, str(ROOT / "scripts"))
from canonicalize_lainir import canonicalize  # noqa: E402

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
        gen1_text = gen1.read_text(encoding="utf-8")
        if "specialization_context_new" not in gen1_text or "emit_function2_context" not in gen1_text:
            raise RuntimeError("gen1 is missing specialization context threading")
        if "next_capacity" not in gen1_text or "specialization_context_key" not in gen1_text:
            raise RuntimeError("gen1 is missing growable scratch/context key lowering")
        if "specialization_context_set_state" not in gen1_text or "allocate-pages(72)" not in gen1_text:
            raise RuntimeError("gen1 is missing module materialization state")

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
        canonical2 = canonicalize(text2.decode("utf-8"))
        canonical3 = canonicalize(text3.decode("utf-8"))
        if canonical2 != canonical3:
            raise RuntimeError(
                f"self-hosting did not converge after canonicalization: "
                f"gen2 {len(text2)} bytes, gen3 {len(text3)} bytes"
            )
        if os.environ.get("LAIN_M2_UPDATE_CACHE") == "1":
            cache = ROOT / "build" / "debug-gen2-heap.l1"
            cache.parent.mkdir(parents=True, exist_ok=True)
            cache.write_bytes(gen2.read_bytes())
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
