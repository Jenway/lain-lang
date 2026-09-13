#!/usr/bin/env python3
"""Execute ordinary compiler pipeline macro expansion, not helper probes."""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from toolchain import seed_exe

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    ("formal_macro_return", "42"),
    ("formal_macro_two_args_return", "42"),
    ("formal_macro_two_declarations_return", "21"),
    ("formal_macro_recursive", "4202"),
    ("formal_macro_missing_argument", "4203"),
    ("formal_macro_extra_argument", "4204"),
    ("formal_macro_caller_argument", "42"),
    ("formal_macro_compound_argument", "42"),
    ("formal_macro_atom_return", "42"),
    ("formal_macro_precedence", "42"),
    ("formal_macro_associativity", "42"),
    ("formal_macro_nested_arguments", "42"),
)



def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, help="explicit isolated compiler bundle")
    args = parser.parse_args()
    if args.compiler is None:
        subprocess.run([sys.executable, ROOT / "scripts/build_lain_compiler.py"], cwd=ROOT, check=True)
    compiler = args.compiler or ROOT / "build/bootstrap/lainc.l1"
    with tempfile.TemporaryDirectory(prefix="bootstrap-macro-expand-") as directory:
        work = Path(directory)
        cases = [(name, ROOT / "scripts/fixtures" / (name + ".lain"), expected) for name, expected in CASES]
        for i, (label, source, expected) in enumerate(cases):
            output = work / f"out{i}.l1"
            compiled = subprocess.run([seed_exe("lainir-seed"), compiler, "compiler_compile_library", output, source,
                                       ROOT / "scripts/fixtures/empty_source.lain"], cwd=ROOT, capture_output=True, text=True, timeout=40)
            if expected.startswith("420"):
                if compiled.returncode == 0 or f"status {expected}" not in compiled.stderr:
                    raise RuntimeError(f"{label}: expected diagnostic {expected}: {compiled.stderr}")
            else:
                if compiled.returncode:
                    raise RuntimeError(f"{label}: {compiled.stderr}")
                subprocess.run([seed_exe("lainir-print"), output, "main"], cwd=ROOT, capture_output=True, check=True)
                executed = subprocess.run([seed_exe("lainir-seed"), "run", output, "main"], cwd=ROOT, capture_output=True, text=True)
                if executed.returncode or executed.stdout.strip() != expected:
                    raise RuntimeError(f"{label}: expected {expected}, got {executed.stdout!r}: {executed.stderr}")
            print(f"PASS {label}: {expected}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
