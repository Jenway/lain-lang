#!/usr/bin/env python3
"""Check that the frozen lainc lowers the canonical compiler source closure."""

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
COMPILER = ROOT / "bootstrap" / "frozen" / "lainc.l1"
MODULES = (
    "src/compiler/tokenizer.lain",
    "src/compiler/syntax.lain",
    "src/compiler/generated_syntax.lain",
    "src/compiler/types.lain",
    "src/compiler/effects.lain",
    "src/compiler/compiler_context.lain",
    "src/compiler/modules.lain",
    "src/compiler/source_workspace.lain",
    "src/compiler/module_artifact.lain",
    "src/compiler/elaborator.lain",
    "src/compiler/l1_ir.lain",
    "src/compiler/l1_unit_builder.lain",
    "src/compiler/l1_verifier.lain",
    "src/compiler/l1_printer.lain",
    "src/compiler/l1_interpreter.lain",
    "src/compiler/meta.lain",
    "src/compiler/lower.lain",
    "src/compiler/workspace.lain",
    "src/compiler/frontend_pipeline.lain",
    "src/compiler/compiler.lain",
    "src/compiler/compiler_core.lain",
    "src/compiler/compiler_driver.lain",
    "src/compiler/compiler_api.lain",
    "src/compiler/lainc.lain",
)


def run(*arguments: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def main() -> int:
    if not COMPILER.exists():
        raise RuntimeError(
            "frozen compiler is missing; run scripts/freeze_lainc_bootstrap.py"
        )
    sources = [ROOT / relative for relative in MODULES]
    with tempfile.TemporaryDirectory(prefix="lainc-source-closure-") as directory:
        output = Path(directory) / "compiler-closure.l1"
        generated = run(
            BOOTSTRAP,
            COMPILER,
            "compiler_compile_library",
            output,
            *sources,
        )
        if generated.returncode:
            raise RuntimeError(generated.stderr or generated.stdout)
        checked = run(CHECK, output)
        if checked.returncode:
            raise RuntimeError(checked.stderr or checked.stdout)
        text = output.read_text(encoding="utf-8")
        if "(error " in text:
            raise RuntimeError("source closure emitted a diagnostic artifact")
        if "comptime_i32" not in text or "callable_phase_runtime" not in text:
            raise RuntimeError("source closure artifact is missing compiler functions")
    print("PASS frozen lainc compiler source closure")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error)
        raise SystemExit(1)
