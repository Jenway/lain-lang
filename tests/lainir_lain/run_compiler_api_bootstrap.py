#!/usr/bin/env python3
"""Exercise full compiler-API Meta instantiation through the frozen lainc."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "seed" / "zig-out" / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""
BOOTSTRAP = BIN / f"lainir-seed{SUFFIX}"
CHECK = BIN / f"lainir-print{SUFFIX}"
INTERPRETER = BIN / f"lainir-seed{SUFFIX}"
COMPILER = ROOT / "src" / "lainir" / "lainc.l1"
FIXTURE = ROOT / "tests" / "lainir_lain" / "fixtures" / "compiler_api_schema.lain"
COMPILER_SOURCES = tuple(sorted((ROOT / "src" / "compiler").glob("*.lain")))
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


def run(*arguments: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments], cwd=ROOT,
        capture_output=True, text=True,
    )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lainc-api-bootstrap-") as directory:
        output = Path(directory) / "compiler-api.l1"
        generated = run(
            BOOTSTRAP, COMPILER, "compiler_compile", output, FIXTURE,
            *COMPILER_SOURCES, *STD_SOURCES,
        )
        if generated.returncode:
            print(generated.stderr or generated.stdout)
            return 1
        checked = run(CHECK, output, "main")
        if checked.returncode:
            print(checked.stderr or checked.stdout)
            return 1
        executed = run(INTERPRETER, "run", output, "main")
        if executed.returncode or executed.stdout.strip() != "1":
            print(executed.stderr or executed.stdout)
            return 1
    print("PASS compiler API bootstrap instantiation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
