#!/usr/bin/env python3
"""Verify that the frozen minimal lainc runs through the C bootstrap host."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "bootstrap" / "zig-out" / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""
BOOTSTRAP = BIN / f"lainir-seed{SUFFIX}"
CHECK = BIN / f"lainir-print{SUFFIX}"
INTERPRETER = BIN / f"lainir-seed{SUFFIX}"
COMPILER = ROOT / "bootstrap" / "frozen" / "lainc.l1"


def run(*arguments: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def require(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode:
        raise RuntimeError(
            f"{label} failed with {result.returncode}:\n"
            f"{result.stderr or result.stdout}"
        )


def compile_and_run(source: Path, expected: str, output: Path) -> None:
    generated = run(BOOTSTRAP, COMPILER, "compiler_compile", output, source)
    require(generated, f"bootstrap compile {source.name}")
    checked = run(CHECK, output, "main")
    require(checked, f"check {source.name}")
    executed = run(INTERPRETER, "run", output, "main")
    require(executed, f"execute {source.name}")
    if executed.stdout.strip() != expected:
        raise RuntimeError(
            f"{source.name} returned {executed.stdout.strip()!r}, "
            f"expected {expected!r}"
        )


def main() -> int:
    if not COMPILER.exists():
        raise RuntimeError(
            "frozen compiler is missing; run scripts/freeze_lainc_bootstrap.py"
        )
    return_42 = ROOT / "tests" / "core" / "pure_lain" / "fixtures" / "return_42.lain"
    module_factory = (
        ROOT / "tests" / "lainir_lain" / "fixtures" / "module_factory_bind.lain"
    )
    module_capture = (
        ROOT / "tests" / "lainir_lain" / "fixtures" / "module_factory_capture.lain"
    )
    with tempfile.TemporaryDirectory(prefix="lainc-bootstrap-") as directory:
        temporary = Path(directory)
        compile_and_run(return_42, "42", temporary / "return_42.l1")
        compile_and_run(module_factory, "42", temporary / "module_factory.l1")
        compile_and_run(module_capture, "42", temporary / "module_capture.l1")
    print("PASS frozen bootstrap lainc")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error)
        raise SystemExit(1)
