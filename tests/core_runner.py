#!/usr/bin/env python3
"""Run the small, contract-oriented core test suites."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(name: str, command: list[str]) -> bool:
    print(f"\n== {name} ==", flush=True)
    result = subprocess.run(command, cwd=ROOT, text=True)
    if result.returncode == 0:
        print(f"PASS {name}", flush=True)
        return True
    print(f"FAIL {name}", flush=True)
    return False


def main() -> int:
    python = sys.executable
    suites = [
        ("core/ast", [python, "tests/core/ast/run_ast_golden.py"]),
        (
            "core/declarations",
            [python, "tests/core/declarations/run_declaration_contract.py"],
        ),
        (
            "core/artifact-cache",
            [python, "tests/core/artifact_cache/run_artifact_cache.py"],
        ),
        ("core/comptime", [python, "tests/core/comptime/run_comptime_contract.py"]),
        ("core/meta", [python, "tests/core/meta/run_meta_contract.py"]),
        ("core/examples", [python, "tests/core/examples/run_examples.py"]),
        ("core/boundaries", [python, "tests/core/boundaries/boundary_lint.py"]),
        (
            "core/bootstrap-execution",
            [python, "tests/core/bootstrap_execution/run_bootstrap_execution.py"],
        ),
        ("core/lainir-contract", [python, "tests/core/lainir_contract/run_contract.py"]),
        ("core/self-hosting", [python, "tests/core/self_hosting/run_self_hosting.py"]),
        ("bootstrap-core", [python, "tests/bootstrap-core/run_bootstrap_core.py"]),
    ]

    passed = 0
    failed = 0
    for name, command in suites:
        if run(name, command):
            passed += 1
        else:
            failed += 1

    print("\nCore summary", flush=True)
    print("============", flush=True)
    print(f"passed suites: {passed}", flush=True)
    print(f"failed suites: {failed}", flush=True)
    if failed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
