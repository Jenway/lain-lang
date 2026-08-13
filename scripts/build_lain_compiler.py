#!/usr/bin/env python3
"""Build the first executable Lain-to-LAIN-IR compiler slice."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
L1CHECK = ROOT / "bootstrap" / "zig-out" / "bin" / (
    "l1check.exe" if os.name == "nt" else "l1check"
)
OUTPUT = ROOT / "build" / "lainir" / "lain_compiler.l1"
MODULES = (
    ROOT / "src" / "lainir" / "tools" / "source.l1",
    ROOT / "src" / "lainir" / "lain" / "raw_ast.l1",
    ROOT / "src" / "lainir" / "lain" / "meta.l1",
    ROOT / "src" / "lainir" / "lain" / "meta_values.l1",
    ROOT / "src" / "lainir" / "lain" / "module_meta.l1",
    ROOT / "src" / "lainir" / "lain" / "eval.l1",
    ROOT / "src" / "lainir" / "lain" / "workspace.l1",
    ROOT / "src" / "lainir" / "lain" / "workspace_cache.l1",
    ROOT / "src" / "lainir" / "lain" / "syntax_units.l1",
    ROOT / "src" / "lainir" / "lain" / "lower_func.l1",
    ROOT / "src" / "lainir" / "lain" / "lower_program.l1",
    ROOT / "src" / "lainir" / "lain" / "meta_type.l1",
    ROOT / "src" / "lainir" / "lain" / "meta_record.l1",
    ROOT / "src" / "lainir" / "lain" / "meta_module.l1",
    ROOT / "src" / "lainir" / "lain" / "meta_import.l1",
    ROOT / "src" / "lainir" / "lain" / "meta_bindings.l1",
    ROOT / "src" / "lainir" / "lain" / "meta_eval_helpers.l1",
    ROOT / "src" / "lainir" / "lain" / "meta_eval_cache.l1",
    ROOT / "src" / "lainir" / "lain" / "meta_eval_bindings.l1",
    ROOT / "src" / "lainir" / "lain" / "meta_eval.l1",
    ROOT / "src" / "lainir" / "lain" / "meta_call.l1",
    ROOT / "src" / "lainir" / "lain" / "meta_collect.l1",
    ROOT / "src" / "lainir" / "lain" / "compiler_api.l1",
    ROOT / "src" / "lainir" / "lain" / "compiler.l1",
)


def run(arguments: list[Path | str]) -> None:
    result = subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def main() -> int:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    run([sys.executable, BUNDLER, "-o", OUTPUT, *MODULES])
    run([L1CHECK, OUTPUT, "compiler_compile"])
    print(OUTPUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"lain compiler build failed: {error}", file=sys.stderr)
        raise SystemExit(1)
