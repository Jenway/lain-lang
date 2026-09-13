#!/usr/bin/env python3
"""Execute ordinary scalar expressions through an explicit compiler bundle."""
from __future__ import annotations
import argparse
import subprocess
import tempfile
import sys
from pathlib import Path
from toolchain import seed_exe
ROOT = Path(__file__).resolve().parents[1]
CASES = ("precedence", "associativity", "nested")
def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    args = parser.parse_args()
    if not args.compiler.is_file():
        print(f"compiler bundle not found: {args.compiler}", file=sys.stderr)
        return 2
    work_root = ROOT / "build"
    work_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="formal-expression-", dir=work_root) as directory:
        for label in CASES:
            output = Path(directory) / (label + ".l1")
            source = ROOT / "scripts/fixtures" / ("formal_expression_" + label + ".lain")
            compiled = subprocess.run([seed_exe("lainir-seed"), args.compiler.resolve(), "compiler_compile_library", output, source,
                                       ROOT / "scripts/fixtures/empty_source.lain"], cwd=ROOT, capture_output=True, text=True, timeout=40)
            if compiled.returncode:
                raise RuntimeError(f"{label}: compile failed: {compiled.stderr}")
            subprocess.run([seed_exe("lainir-print"), output, "main"], cwd=ROOT, capture_output=True, check=True)
            if "#eval" not in output.read_text(encoding="utf-8"):
                raise RuntimeError(f"{label}: expected explicit compile-time arithmetic IR")
            result = subprocess.run([seed_exe("lainir-seed"), "run", output, "main"], cwd=ROOT, capture_output=True, text=True)
            if result.returncode or result.stdout.strip() != "42":
                raise RuntimeError(f"{label}: expected 42, got {result.stdout!r}: {result.stderr}")
            print(f"PASS formal expression {label}: 42")
        for label, expression in (("trailing_operator", "20 +"), ("unknown_operand", "(20 + 1) + unknown")):
            source = Path(directory) / (label + ".lain")
            output = Path(directory) / (label + ".l1")
            source.write_text("let main = std::func() -> i64 { return " + expression + "; };\n", encoding="utf-8")
            compiled = subprocess.run([seed_exe("lainir-seed"), args.compiler.resolve(), "compiler_compile_library", output, source,
                                       ROOT / "scripts/fixtures/empty_source.lain"], cwd=ROOT, capture_output=True, text=True, timeout=40)
            if compiled.returncode == 0 or "status 5203" not in compiled.stderr or output.exists():
                raise RuntimeError(f"{label}: expected diagnostic 5203 and no artifact: {compiled.stderr}")
            print(f"PASS formal expression {label}: diagnostic 5203")
    return 0
if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
