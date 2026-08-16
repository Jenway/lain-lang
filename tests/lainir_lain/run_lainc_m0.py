#!/usr/bin/env python3
"""M0/M1 acceptance: the Lain-written lainc compiles return_42.

The Lain-written compiler (src/lainc/lainc.lain) is compiled by the frozen
lainc and executed by the seed interpreter.  Its main entry compiles a
hard-coded `return 42` source and verifies the emitted LAIN-IR text inside
the Lain layer with an exact byte comparison; it returns 1 when correct.

The verified product text is then written to a file, passed through
lainir-print (l1check) and executed, proving the emitted LAIN-IR is valid
and runs to 42.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "seed" / "zig-out" / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""
SEED = BIN / f"lainir-seed{SUFFIX}"
PRINT = BIN / f"lainir-print{SUFFIX}"
FROZEN = ROOT / "src" / "lainir" / "lainc.l1"
LAINC = ROOT / "src" / "lainc" / "lainc.lain"
STD_SOURCES = (
    ROOT / "std" / "memory_model.lain",
    ROOT / "std" / "allocation.lain",
    ROOT / "std" / "bounds.lain",
    ROOT / "std" / "effect.lain",
    ROOT / "std" / "core" / "vec.lain",
    ROOT / "std" / "core" / "string.lain",
    ROOT / "std" / "core" / "arena_min.lain",
    ROOT / "std" / "core" / "slice.lain",
    ROOT / "std" / "core" / "source.lain",
    ROOT / "std" / "core" / "memory.lain",
)
# The exact LAIN-IR text that src/lainc/lainc.lain emits and verifies.
PRODUCT = "#proc main() -> #bits<32> {\n  #return 42\n}\n"


def run(*arguments: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(a) for a in arguments], cwd=ROOT, capture_output=True, text=True
    )


def main() -> int:
    if not SEED.exists() or not PRINT.exists():
        raise RuntimeError("build seed first: cd seed && zig build")
    with tempfile.TemporaryDirectory(prefix="lainc-m0-") as directory:
        tmp = Path(directory)
        artifact = tmp / "lainc.l1"
        compiled = run(SEED, FROZEN, "compiler_compile", artifact, LAINC, *STD_SOURCES)
        if compiled.returncode:
            raise RuntimeError(compiled.stderr or compiled.stdout)
        verified = run(SEED, "run", artifact, "main")
        if verified.returncode or verified.stdout.strip() != "1":
            raise RuntimeError(f"lainc main verification failed: {verified.stdout!r} {verified.stderr}")
        product = tmp / "product.l1"
        product.write_text(PRODUCT, encoding="utf-8", newline="")
        checked = run(PRINT, product, "main")
        if checked.returncode:
            raise RuntimeError(checked.stderr or checked.stdout)
        executed = run(SEED, "run", product, "main")
        if executed.returncode or executed.stdout.strip() != "42":
            raise RuntimeError(f"product did not run to 42: {executed.stdout!r}")
    print("PASS Lain-written lainc compiles return_42, product verifies and runs to 42")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
