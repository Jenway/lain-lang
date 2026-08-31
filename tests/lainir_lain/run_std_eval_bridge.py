#!/usr/bin/env python3
"""Verify the bootstrap stdlib compile-time evaluator crosses #eval."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
BIN = ROOT / "seed" / "zig-out" / "bin"
SEED = BIN / "lainir-seed.exe"
PRINT = BIN / "lainir-print.exe"
RUNNER = ROOT / "scripts" / "run_lain_compiler.py"
COMBINED = ROOT / "build" / "lainir" / "lain_compiler.l1"
CORE = ROOT / "build" / "lainir" / "lain_compiler_core.l1"
STD = ROOT / "build" / "lainir" / "bootstrap_std.l1"
FIXTURE = ROOT / "tests" / "lainir_lain" / "fixtures" / "consteval_and_function.lain"


def run(*args: pathlib.Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(arg) for arg in args], cwd=ROOT, capture_output=True, text=True)


def require(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode:
        raise RuntimeError(f"{label}: {result.stderr.strip() or result.stdout.strip()}")


def main() -> int:
    try:
        require(run(sys.executable, ROOT / "scripts" / "build_lain_compiler.py"), "build")
        std_text = STD.read_text(encoding="utf-8")
        if "\n#proc program_eval_consteval_group(" not in std_text:
            raise RuntimeError("bootstrap stdlib has no evaluator entry")
        if "#eval {" not in std_text:
            raise RuntimeError("bootstrap stdlib evaluator does not use #eval")
        if "\n#proc program_eval_consteval_group(" in CORE.read_text(encoding="utf-8"):
            raise RuntimeError("compiler core unexpectedly owns the evaluator entry")
        require(run(PRINT, STD, "program_eval_consteval_group"), "stdlib verifier")
        with tempfile.TemporaryDirectory(prefix="lain-std-eval-bridge-") as directory:
            output = pathlib.Path(directory) / "consteval.l1"
            require(run(sys.executable, RUNNER, "-o", output, FIXTURE), "compile consteval")
            require(run(PRINT, output, "main"), "compiled verifier")
            executed = run(SEED, "run", output, "main")
            require(executed, "compiled execution")
            if executed.stdout.strip() != "42":
                raise RuntimeError(f"consteval result {executed.stdout.strip()!r}, expected '42'")
        print("PASS bootstrap stdlib #eval evaluator bridge")
        return 0
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
