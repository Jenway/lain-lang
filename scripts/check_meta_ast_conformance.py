#!/usr/bin/env python3
"""Compare first-generation RawAst output with formal std::meta output."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "seed" / "zig-out" / "bin" / "lainir-seed.exe"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
COMPILER = ROOT / "build" / "lainir" / "lain_compiler.l1"
CORE = ROOT / "build" / "lainir" / "lain_compiler_core.l1"
FORMAL = ROOT / "build" / "lainir" / "formal_stdlib.l1"
PROBE = ROOT / "scripts" / "fixtures" / "formal_meta_ast_dump_probe.l1"
SOURCE = ROOT / "scripts" / "fixtures" / "formal_ast_conformance.lain"
SECOND_SOURCE = ROOT / "scripts" / "fixtures" / "formal_constant_return.lain"
FIRST_OUTPUT = ROOT / "build" / "lainir" / "first_generation_ast.dump"
FORMAL_BUNDLE = ROOT / "build" / "lainir" / "formal_meta_ast_dump_bundle.l1"
FORMAL_OUTPUT = ROOT / "build" / "lainir" / "formal_meta_ast.dump"


def run(arguments: list[Path | str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def main() -> int:
    first = run(
        [
            SEED,
            "interpreter",
            COMPILER,
            "lain_raw_ast_dump",
            FIRST_OUTPUT,
            SOURCE,
            SECOND_SOURCE,
        ]
    )
    if first.returncode:
        raise RuntimeError(first.stderr.strip() or first.stdout.strip())
    bundle = run([sys.executable, BUNDLER, "-o", FORMAL_BUNDLE, CORE, FORMAL, PROBE])
    if bundle.returncode:
        raise RuntimeError(bundle.stderr.strip() or bundle.stdout.strip())
    formal = run(
        [
            SEED,
            "interpreter",
            FORMAL_BUNDLE,
            "main",
            FORMAL_OUTPUT,
            SOURCE,
            SECOND_SOURCE,
        ]
    )
    if formal.returncode:
        raise RuntimeError(formal.stderr.strip() or formal.stdout.strip())
    first_text = FIRST_OUTPUT.read_text(encoding="utf-8")
    formal_text = FORMAL_OUTPUT.read_text(encoding="utf-8")
    if first_text != formal_text:
        print("first-generation AST:", repr(first_text), file=sys.stderr)
        print("formal Meta AST:", repr(formal_text), file=sys.stderr)
        raise RuntimeError("canonical AST output differs")
    print("PASS first-generation/formal Meta canonical AST")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"canonical AST conformance failed: {error}", file=sys.stderr)
        raise SystemExit(1)
