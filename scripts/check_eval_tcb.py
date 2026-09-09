#!/usr/bin/env python3
"""Exercise the C seed contract for temporary-TCB #eval execution."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "seed" / "zig-out" / "bin" / "lainir-seed.exe"
FIXTURES = ROOT / "scripts" / "fixtures"


def run(name: str, *options: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(SEED), "run", *options, str(FIXTURES / name), "main"],
        text=True,
        capture_output=True,
        check=False,
    )


def require_success(name: str, expected: str, *options: str) -> None:
    result = run(name, *options)
    if result.returncode != 0 or result.stdout.strip() != expected:
        raise SystemExit(
            f"{name}: expected success output {expected!r}; "
            f"got code={result.returncode}, stdout={result.stdout!r}, "
            f"stderr={result.stderr!r}"
        )


def require_trap(name: str, text: str, *options: str) -> None:
    result = run(name, *options)
    if result.returncode == 0 or text not in result.stderr:
        raise SystemExit(
            f"{name}: expected trap containing {text!r}; "
            f"got code={result.returncode}, stdout={result.stdout!r}, "
            f"stderr={result.stderr!r}"
        )


def main() -> int:
    if not SEED.is_file():
        raise SystemExit(f"build seed first: {SEED}")
    require_success("eval_tcb_capture.l1", "42")
    require_success("eval_tcb_argument_capture.l1", "42")
    require_success("eval_tcb_nested.l1", "42")
    require_success("eval_tcb_context_type.l1", "0")
    require_trap(
        "eval_tcb_alloca_escape.l1", "#alloca address escaped #eval activation"
    )
    require_trap(
        "eval_tcb_capture.l1", "call-depth limit exceeded", "--max-call-depth", "1"
    )
    require_trap(
        "eval_tcb_allocation_budget.l1",
        "allocation limit exceeded",
        "--max-alloc-bytes",
        "12",
    )
    print("PASS temporary-TCB #eval contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
