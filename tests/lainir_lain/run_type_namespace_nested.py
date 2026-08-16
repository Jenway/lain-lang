#!/usr/bin/env python3
"""Check nested type-namespace calls in a factory-produced type."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "bootstrap" / "zig-out" / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""
BOOTSTRAP = BIN / f"lainir-interpreter{SUFFIX}"
CHECK = BIN / f"lainir-print{SUFFIX}"
FIXTURE = ROOT / "tests" / "lainir_lain" / "fixtures" / "type_namespace_nested.lain"
COMPILER = ROOT / "bootstrap" / "frozen" / "lainc.l1"
SOURCES = tuple(sorted((ROOT / "src" / "compiler").glob("*.lain"))) + (
    ROOT / "std" / "memory_model.lain",
    ROOT / "std" / "allocation.lain",
    ROOT / "std" / "bounds.lain",
    ROOT / "std" / "effect.lain",
    ROOT / "std" / "core" / "vec.lain",
    ROOT / "std" / "core" / "string.lain",
    ROOT / "std" / "core" / "arena_min.lain",
)


def run(*args: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(arg) for arg in args], cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-type-namespace-") as directory:
        output = Path(directory) / "type-namespace.l1"
        generated = run(BOOTSTRAP, COMPILER, "compiler_compile", output, FIXTURE, *SOURCES)
        if generated.returncode:
            print(generated.stderr or generated.stdout)
            return 1
        checked = run(CHECK, output, "main")
        if checked.returncode:
            print(checked.stderr or checked.stdout)
            return 1
    print("PASS nested type namespace specialization")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
