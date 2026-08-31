#!/usr/bin/env python3
"""Compile a formal std::meta module through the bootstrap compiler artifact."""

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
FIXTURE = ROOT / "tests" / "lainir_lain" / "fixtures" / "stdlib_meta_bootstrap.lain"
BOOTSTRAP = ROOT / "build" / "debug-gen2-heap.l1"
FROZEN = ROOT / "src" / "lainir" / "lainc.l1"
LAINC = ROOT / "src" / "lainc" / "lainc.lain"
STD = tuple(
    ROOT / name
    for name in (
        "std/effect.lain",
        "std/allocation.lain",
        "std/bounds.lain",
        "std/core/vec.lain",
        "std/core/string.lain",
        "std/core/arena_min.lain",
        "std/core/slice.lain",
        "std/core/source.lain",
        "std/core/memory.lain",
        "std/memory_model.lain",
        "std/meta.lain",
    )
)


def run(*args: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(arg) for arg in args], cwd=ROOT, capture_output=True, text=True
    )


def main() -> int:
    if not BOOTSTRAP.exists():
        raise RuntimeError(f"missing bootstrap compiler artifact: {BOOTSTRAP}")
    with tempfile.TemporaryDirectory(prefix="lain-stdlib-bootstrap-") as directory:
        temporary = Path(directory)
        product = temporary / "stdlib-meta.l1"
        if os.environ.get("LAIN_META_USE_CACHED_GEN2") == "1" and BOOTSTRAP.exists():
            compiler = BOOTSTRAP
        else:
            gen1 = temporary / "gen1.l1"
            gen2 = temporary / "gen2.l1"
            first = run(SEED, FROZEN, "compiler_compile", gen1, LAINC)
            if first.returncode:
                raise RuntimeError(first.stderr or first.stdout)
            second = run(SEED, "interpreter", gen1, "compiler_compile", gen2, LAINC)
            if second.returncode:
                raise RuntimeError(second.stderr or second.stdout)
            compiler = gen2
        generated = run(
            SEED,
            "interpreter",
            compiler,
            "compiler_compile_library",
            product,
            *STD,
            FIXTURE,
        )
        if generated.returncode:
            raise RuntimeError(generated.stderr or generated.stdout)
        checked = run(PRINT, product, "main")
        if checked.returncode:
            raise RuntimeError(checked.stderr or checked.stdout)
        executed = run(SEED, "run", product, "main")
        if executed.returncode or executed.stdout.strip() != "0":
            raise RuntimeError(executed.stderr or executed.stdout)
    print("PASS formal std::meta compiled by bootstrap artifact")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error)
        raise SystemExit(1)
