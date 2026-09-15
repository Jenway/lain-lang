#!/usr/bin/env python3
"""Execute the library type-window scanner against independent source offsets."""
from __future__ import annotations
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
from bundle_lainir import bundle
from toolchain import seed_exe

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    ("qualified_meta", "std::type", "type"),
    ("qualified_name", "namespace::Record", "Record"),
    ("reference_meta", "&std::type", "type"),
    ("mutable_reference", "&mut namespace::Record", "Record"),
    ("unqualified", "i64", "i64"),
    ("type_call", "Box(i32)", "(i32)"),
    ("qualified_before_comma", "std::type, other: i64", "type"),
)

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path)
    args = parser.parse_args()
    if args.compiler is None:
        subprocess.run([sys.executable, ROOT / "scripts/build_lain_compiler.py"], cwd=ROOT, check=True, capture_output=True)
    compiler = args.compiler or ROOT / "build/bootstrap/lainc.l1"
    with tempfile.TemporaryDirectory(prefix="type-expression-end-", dir=ROOT / "build") as directory:
        work = Path(directory)
        probe = work / "compiler.l1"
        probe.write_text(bundle([compiler.resolve(), ROOT / "scripts/fixtures/type_expression_end_probe.l1"]), encoding="utf-8")
        for name, expression, terminal in CASES:
            prefix = "let f = std::func(value: "
            text = prefix + expression + ") -> i64 { return 0; };\n"
            expected = text.index(terminal, len(prefix))
            source = work / (name + ".lain")
            source.write_text(text, encoding="utf-8")
            output = work / (name + ".txt")
            result = subprocess.run(
                [seed_exe("lainir-seed"), "interpreter", probe, "type_expression_end_probe", output,
                 source, ROOT / "scripts/fixtures/empty_source.lain"],
                cwd=ROOT, capture_output=True, text=True, timeout=30,
            )
            actual = output.read_text() if output.is_file() else "missing"
            if result.returncode or actual != str(expected):
                raise RuntimeError(f"{name}: expected terminal offset {expected}, got {actual}: {result.stderr}")
            print(f"PASS type expression end {name}: {actual}")
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
