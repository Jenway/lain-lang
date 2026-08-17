#!/usr/bin/env python3
"""src/lainc stage-B acceptance: std external capability stubs.

Verifies with gen2:
  - import("std::...") binds a stub module (no host source needed)
  - std stub member calls lower to literal 0 (capability placeholder)
  - `-> Module` factories bind meta and emit no runtime proc
  - src/compiler-archive/tokenizer.lain compiles to a verifier-clean
    product (std imports + module factory + effect clauses + || chains)
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
TOKENIZER = ROOT / "src" / "compiler-archive" / "tokenizer.lain"


def run(*arguments: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(a) for a in arguments], cwd=ROOT, capture_output=True, text=True
    )


def main() -> int:
    if not SEED.exists() or not PRINT.exists():
        raise RuntimeError("build seed first: cd seed && zig build")
    with tempfile.TemporaryDirectory(prefix="lainc-stdb-") as directory:
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

        # 1. std import stub: member calls lower to 0, product runs to 42.
        out = tmp / "std_client.l1"
        res = run(SEED, "interpreter", gen2, "compiler_compile_library", out, FIX / "std_client.lain")
        if res.returncode:
            raise RuntimeError(f"std_client: {res.stderr or res.stdout}")
        checked = run(PRINT, out, "main")
        if checked.returncode:
            raise RuntimeError(f"std_client l1check: {checked.stderr}")
        text = out.read_text(encoding="utf-8")
        assert "shape_id" not in text, "std stub call was not lowered to 0"
        ran = run(SEED, "run", out, "main")
        if ran.returncode:
            raise RuntimeError(f"std_client run: {ran.stderr or ran.stdout}")
        assert ran.stdout.strip() == "42", f"std_client run = {ran.stdout!r}"
        print("PASS std import stub (memory_model.shape_id -> 0, run 42)")

        # 2. tokenizer.lain: std imports + module factory + effect clauses
        #    + || chains compile to a verifier-clean product.
        entry = tmp / "entry_token.lain"
        with open(entry, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(
                'let tokenizer: Module = import("packages::lain::compiler::tokenizer");\n'
                "let main = std::func() -> i32 {\n    return 0;\n};\n"
            )
        out2 = tmp / "tokenizer.l1"
        res = run(SEED, "interpreter", gen2, "compiler_compile_library", out2, TOKENIZER, entry)
        if res.returncode:
            raise RuntimeError(f"tokenizer: {res.stderr or res.stdout}")
        checked = run(PRINT, out2, "main")
        if checked.returncode:
            raise RuntimeError(f"tokenizer l1check: {checked.stderr}")
        text = out2.read_text(encoding="utf-8")
        assert "f0_tokenizer_" not in text, "module factory emitted a runtime proc"
        print("PASS tokenizer.lain compiles, l1check clean, no factory proc")
    print("PASS src/lainc stage B: std external capability stubs")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
