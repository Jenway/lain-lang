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
FIXTURES = (
    (ROOT / "scripts" / "fixtures" / "formal_duplicate_module.lain", "3013"),
    (ROOT / "scripts" / "fixtures" / "formal_duplicate_struct.lain", "3008"),
)


def run(
    compiler: Path,
    output: Path,
    fixture: Path,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(SEED),
            "interpreter",
            str(compiler),
            "compiler_compile_library",
            str(output),
            str(COMPILER_API),
            str(fixture),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def main() -> int:
    work = ROOT / "build" / "lainir"
    compilers = (
        ("bootstrap", FIRST_GENERATION),
        ("formal", FORMAL_ABI),
    )
    for fixture, expected in FIXTURES:
        for label, compiler in compilers:
            output = work / f"{label}_{fixture.stem}.l1"
            result = run(compiler, output, fixture)
            detail = result.stdout + result.stderr
            if result.returncode == 0 or expected not in detail:
                raise RuntimeError(
                    f"{label} Meta did not reject {fixture.name} "
                    f"with {expected}: {detail.strip()}"
                )
    print("PASS bootstrap/formal Meta module/struct validation (3013, 3008)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"Meta module validation failed: {error}")
        raise SystemExit(1)
