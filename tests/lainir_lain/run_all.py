#!/usr/bin/env python3
"""Run the LAIN-IR-written Lain frontend slices."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]


def main() -> int:
    scripts = (
        "run_raw_ast.py",
        "run_meta_record.py",
        "run_meta_module.py",
        "run_meta_eval.py",
        "run_lower_record.py",
        "run_compiler_compile.py",
        "run_bootstrap_lainc.py",
        "run_compiler_source_closure.py",
        "run_workspace.py",
        "run_driver.py",
        "run_lainc_m0.py",
    )
    for script in scripts:
        result = subprocess.run(
            [sys.executable, str(pathlib.Path(__file__).with_name(script))],
            cwd=ROOT,
            text=True,
        )
        if result.returncode:
            return result.returncode
    print("Lain frontend slices: all enabled checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
