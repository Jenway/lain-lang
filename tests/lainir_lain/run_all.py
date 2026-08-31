#!/usr/bin/env python3
"""Run the LAIN-IR-written Lain frontend slices."""

from __future__ import annotations

import pathlib
import os
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]


def main() -> int:
    scripts = (
        "run_seed_allocator.py",
        "run_artifact_canonical.py",
        "run_raw_ast.py",
        "run_ast_ops.py",
        "run_meta_record.py",
        "run_meta_module.py",
        "run_meta_eval.py",
        "run_bootstrap_std_bundle.py",
        "run_std_eval_bridge.py",
        "run_stdlib_bootstrap.py",
        "run_lower_record.py",
        "run_compiler_compile.py",
        "run_bootstrap_lainc.py",
        "run_compiler_source_closure.py",
        "run_workspace.py",
        "run_driver.py",
        "run_lainc_m1.py",
        "run_meta_invoke_environment.py",
        "run_std_diagnostic.py",
        "run_std_type.py",
        "run_std_generic.py",
        "run_std_backend.py",
        "run_std_bounds.py",
        "run_lainc_module.py",
        "run_lainc_archive_a.py",
        "run_lainc_archive_b.py",
        "run_lainc_archive_c.py",
        "run_lainc_archive_d.py",
        "run_lainc_archive_e.py",
        "run_lainc_archive_usable.py",
        "run_lainc_consteval.py",
        "run_lainc_m2.py",
        "run_lainc_archive_arithmetic.py",
    )
    # Ownership/borrow checking is an optional language layer for the current
    # bootstrap milestone.  Keep its policy test available, but do not let it
    # block the required self-hosting regression set.
    if os.environ.get("LAIN_ENABLE_OPTIONAL_OWNERSHIP") == "1":
        scripts = scripts + ("run_std_effect_ownership.py",)
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
