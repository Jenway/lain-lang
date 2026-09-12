#!/usr/bin/env python3
"""Run the repeatable baseline for the lainc -> LAINIR API migration."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHECKS = (
    ("source boundary", "check_lainc_lainir_api.py", "--final"),
    ("backend manifest", "check_backend_manifest.py"),
    ("backend ABI contract", "check_backend_abi_contract.py"),
    ("seed backend adapter", "check_seed_backend_adapter.py"),
    ("native backend migration", "check_native_backend_migration.py"),
    ("historical native C procedure diff", "check_native_backend_canonical_diff.py"),
    ("provider behavior", "check_lainir_api_behavior.py"),
    ("compile context contract", "check_compile_context.py"),
    ("LAIN-VM single TCB contract", "check_lain_vm_contract.py"),
    ("LAINIR physical safety", "check_lainir_physical_safety.py"),
    ("meta form recognition via stdlib", "check_meta_form_swap.py"),
    ("compiler fixture parity", "check_stdlib_conformance.py"),
    ("function signature", "check_function_signature.py"),
    ("function declaration shape", "check_function_shape_conformance.py"),
    ("typed input effects", "check_input_effects.py"),
    ("program entry contract", "check_program_entry.py"),
    ("bits type width", "check_bits_type.py"),
    ("compiler artifact snapshots", "check_lainc_lainir_api_snapshots.py"),
    ("source-closure determinism", "check_srclainc_artifact.py"),
    ("gen2/gen3 self-host", "run_lainir_self_host.py"),
    # The remaining runnable gates.  They were absent from this list even
    # though they pass, so the "main entry" ran 20 of the 37 check scripts and
    # a green baseline said nothing about the other 17.  They come last
    # because the steps above build the artifacts they read: run_lainir_self_host
    # and build_srclainc drive build_lain_compiler, which writes
    # build/bootstrap/*.
    ("bootstrap consteval", "check_bootstrap_consteval.py"),
    ("bootstrap LAINVM api", "check_bootstrap_vm_api.py"),
    ("eval temporary TCB", "check_eval_tcb.py"),
    ("lain backend ABI", "check_lain_backend_abi.py"),
    ("LAINVM contract boundary", "check_lainvm_boundary.py"),
    ("meta AST conformance", "check_meta_ast_conformance.py"),
    ("meta module validation", "check_meta_module_validation.py"),
    ("std type binding", "check_std_type.py"),
    ("stdlib swap", "check_stdlib_swap.py"),
    ("policy conformance", "check_policy_conformance.py"),
    ("core/stdlib boundary", "check_lainir_boundaries.py"),
    ("LAINIR compiler end to end", "check_lainir_compiler.py"),
    ("backend C shape", "check_backend_c_shape.py"),
    ("bootstrap release packaging", "check_lainc_bootstrap_release.py"),
    # Reads the snapshot that the release step above writes, so it must follow
    # it rather than run standalone.
    ("bootstrap snapshot", "check_lainc_bootstrap_snapshot.py", "build/bootstrap/lainc.l1"),
)

def main() -> int:
    for label, script, *arguments in CHECKS:
        result = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / script), *arguments],
            cwd=ROOT,
            text=True,
        )
        if result.returncode:
            print(f"FAIL API migration baseline: {label}", file=sys.stderr)
            return result.returncode
    print("PASS lainc -> LAINIR API migration baseline")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
