#!/usr/bin/env python3
"""src/lainc stage-D acceptance: factory body language surface.

Verifies with gen2 that a module factory body compiles and runs:
  - factory members with bare cross-calls (classify -> is_space) resolve
    to f0_<factory>_<member> labels
  - record types (std::struct in the factory body), || chains, else,
    unresolved field access all lower to a verifier-clean product
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
    with tempfile.TemporaryDirectory(prefix="lainc-surf-") as directory:
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

        out = tmp / "surface.l1"
        res = run(SEED, "interpreter", gen2, "compiler_compile_library", out, FIX / "t2.lain", FIX / "app_t2.lain")
        if res.returncode:
            raise RuntimeError(f"surface: {res.stderr or res.stdout}")
        checked = run(PRINT, out, "main")
        if checked.returncode:
            raise RuntimeError(f"surface l1check: {checked.stderr}")
        text = out.read_text(encoding="utf-8")
        for label in ("f0_make_process", "f0_make_classify", "f0_make_is_space", "f0_make_span_len"):
            assert f"#proc {label}(" in text, f"missing {label}"
        assert "#call f0_make_classify(37)" in text, "bare cross-call lost factory prefix"
        assert "#call f0_make_is_space(%byte)" in text, "is_space call not prefixed"
        ran = run(SEED, "run", out, "main")
        if ran.returncode:
            raise RuntimeError(f"surface run: {ran.stderr or ran.stdout}")
        assert ran.stdout.strip() == "42", f"surface run = {ran.stdout!r}"
        print("PASS factory body surface (classify -> is_space -> 42)")
    print("PASS src/lainc stage D: factory body language surface")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
