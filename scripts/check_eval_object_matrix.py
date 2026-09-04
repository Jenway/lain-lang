#!/usr/bin/env python3
"""Verify the four EvalResult kinds through an actual LAIN-IR #eval call."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
CORE = ROOT / "build" / "lainir" / "lain_compiler_core.l1"
BOOTSTRAP = ROOT / "build" / "lainir" / "bootstrap_std.l1"
PROBE = ROOT / "scripts" / "fixtures" / "lainir_eval_object_matrix_probe.l1"
SOURCE = ROOT / "scripts" / "fixtures" / "formal_constant_return.lain"
API = ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"
BUNDLE = ROOT / "build" / "lainir" / "lainir_eval_object_matrix.l1"
OUTPUT = ROOT / "build" / "lainir" / "lainir_eval_object_matrix.out"


def run(arguments: list[Path | str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def main() -> int:
    bundled = run([sys.executable, BUNDLER, "-o", BUNDLE, CORE, BOOTSTRAP, PROBE])
    if bundled.returncode:
        raise RuntimeError(bundled.stderr.strip() or bundled.stdout.strip())
    result = run(
        [
            SEED,
            "interpreter",
            BUNDLE,
            "main",
            OUTPUT,
            SOURCE,
            API,
        ]
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())
    if OUTPUT.read_text(encoding="utf-8") != "eval_matrix=1\n":
        raise RuntimeError("EvalResult kind matrix did not pass")
    print("PASS LAIN-IR #eval scalar/type/module/AST result matrix")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"#eval object matrix failed: {error}", file=sys.stderr)
        raise SystemExit(1)
