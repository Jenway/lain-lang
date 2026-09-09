#!/usr/bin/env python3
"""Prove that native lainc emits the same artifact twice for one input."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from prove_lainc_fixed_point import prove


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "scripts" / "fixtures" / "formal_call_return.lain"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_native_lainc_determinism.py <lainc.exe>", file=sys.stderr)
        return 2
    compiler = Path(sys.argv[1])
    if not compiler.is_absolute():
        compiler = ROOT / compiler
    if not compiler.exists():
        print(f"missing native compiler: {compiler}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="native-lainc-determinism-") as directory:
        root = Path(directory)
        outputs = (root / "first.l1", root / "second.l1")
        for output in outputs:
            result = subprocess.run(
                [str(compiler), "-o", str(output), str(FIXTURE)],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            if result.returncode != 0:
                print(f"native compiler failed: exit={result.returncode}", file=sys.stderr)
                if result.stderr:
                    print(result.stderr, file=sys.stderr)
                return 1
        report = prove(*outputs)

    required = (
        report["canonical_equal"],
        report["extern_equal"],
        report["procedure_labels_equal"],
        not report["header_mismatches"],
        not report["body_mismatches"],
    )
    if not all(required):
        print(f"FAIL native artifact determinism: {report}", file=sys.stderr)
        return 1
    print(
        "PASS native lainc artifact determinism "
        f"({report['procedure_count'][0]} procedures)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
