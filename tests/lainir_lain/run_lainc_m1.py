#!/usr/bin/env python3
"""M1 acceptance: the Lain-written lainc compiles add+main to LAIN-IR.

The Lain-written compiler (src/lainc/lainc.lain) is compiled by the frozen
lainc and executed by the seed interpreter.  Its main entry compiles the
hard-coded add+main source and writes the emitted LAIN-IR to the artifact
stream (product.l1).

The artifact text is verified byte-for-byte against the expected LAIN-IR,
then passed through lainir-print (l1check) and executed, proving the emitted
LAIN-IR is valid and runs to 42.
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
# The exact LAIN-IR text that src/lainc/lainc.lain emits for add+main.
PRODUCT = (
    "#extern #proc bootstrap.allocate-pages(#bits<64> %size) -> #addr;\n\n"
    "#proc f0_add(#bits<32> %x, #bits<32> %y) -> #bits<32> {\n"
    "  #return #add(%x, %y)\n"
    "}\n"
    "#proc main() -> #bits<32> {\n"
    "  #return #call f0_add(40, 2)\n"
    "}\n"
)


def run(*arguments: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(a) for a in arguments], cwd=ROOT, capture_output=True, text=True
    )


def main() -> int:
    if not SEED.exists() or not PRINT.exists():
        raise RuntimeError("build seed first: cd seed && zig build")
    with tempfile.TemporaryDirectory(prefix="lainc-m1-") as directory:
        tmp = Path(directory)
        artifact = tmp / "lainc.l1"
        compiled = run(SEED, FROZEN, "compiler_compile", artifact, LAINC, *STD_SOURCES)
        if compiled.returncode:
            raise RuntimeError(compiled.stderr or compiled.stdout)
        checked = run(PRINT, artifact, "main")
        if checked.returncode:
            raise RuntimeError(checked.stderr or checked.stdout)
        product = tmp / "product.l1"
        empty = tmp / "empty.lain"
        empty.write_text("", encoding="utf-8")
        emitted = run(SEED, "interpreter", artifact, "main", product, empty)
        if emitted.returncode:
            raise RuntimeError(emitted.stderr or emitted.stdout)
        with open(product, "r", encoding="utf-8", newline="") as handle:
            text = handle.read()
        if text != PRODUCT:
            raise RuntimeError(
                f"lainc product mismatch: expected {len(PRODUCT)} bytes, "
                f"got {len(text)} bytes:\n{text!r}"
            )
        product_checked = run(PRINT, product, "main")
        if product_checked.returncode:
            raise RuntimeError(product_checked.stderr or product_checked.stdout)
        executed = run(SEED, "run", product, "main")
        if executed.returncode or executed.stdout.strip() != "42":
            raise RuntimeError(f"product did not run to 42: {executed.stdout!r}")
    print("PASS Lain-written lainc compiles add+main, product verifies and runs to 42")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
