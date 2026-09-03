#!/usr/bin/env python3
"""Compare module-shape validation in bootstrap and formal std::meta."""

from __future__ import annotations

import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "seed" / "zig-out" / "bin" / "lainir-seed.exe"
FIRST_GENERATION = ROOT / "build" / "lainir" / "lain_compiler.l1"
FORMAL_ABI = ROOT / "build" / "lainir" / "formal_stdlib_abi_probe.l1"
COMPILER_API = ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"
FIXTURE = ROOT / "scripts" / "fixtures" / "formal_duplicate_module.lain"


def run(compiler: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(SEED),
            "interpreter",
            str(compiler),
            "compiler_compile_library",
            str(output),
            str(COMPILER_API),
            str(FIXTURE),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def main() -> int:
    work = ROOT / "build" / "lainir"
    results = (
        ("bootstrap", FIRST_GENERATION, work / "bootstrap_duplicate_module.l1"),
        ("formal", FORMAL_ABI, work / "formal_duplicate_module.l1"),
    )
    for label, compiler, output in results:
        result = run(compiler, output)
        detail = result.stdout + result.stderr
        if result.returncode == 0 or "3013" not in detail:
            raise RuntimeError(
                f"{label} Meta did not reject duplicate module member: "
                f"{detail.strip()}"
            )
    print("PASS bootstrap/formal Meta module validation (3013)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"Meta module validation failed: {error}")
        raise SystemExit(1)
