#!/usr/bin/env python3
"""M4.1 acceptance for the Meta-owned backend input contract."""

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
FIXTURE = Path(__file__).parent / "fixtures" / "std_backend.lain"


def run(*args: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(arg) for arg in args], cwd=ROOT, capture_output=True, text=True
    )


def checked(*args: Path | str) -> subprocess.CompletedProcess[str]:
    result = run(*args)
    if result.returncode:
        raise RuntimeError(result.stderr or result.stdout)
    return result


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lainc-std-backend-") as directory:
        tmp = Path(directory)
        product = tmp / "std_backend.l1"
        cached = ROOT / "build" / "debug-gen2-heap.l1"
        if os.environ.get("LAIN_META_USE_CACHED_GEN2") == "1" and cached.exists():
            gen2 = cached
        else:
            gen1 = tmp / "gen1.l1"
            gen2 = tmp / "gen2.l1"
            checked(SEED, FROZEN, "compiler_compile", gen1, LAINC)
            checked(SEED, "interpreter", gen1, "compiler_compile", gen2, LAINC)
        checked(
            SEED,
            "interpreter",
            gen2,
            "compiler_compile_library",
            product,
            ROOT / "std" / "backend.lain",
            FIXTURE,
        )
        checked(PRINT, product, "main")
        result = checked(SEED, "run", product, "main")
        if result.stdout.strip() != "42":
            raise RuntimeError(f"std backend result != 42: {result.stdout!r}")
    print("PASS std::backend target/ABI/extern contract")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        raise SystemExit(str(error))
