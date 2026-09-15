#!/usr/bin/env python3
"""Rebuild the Lain compiler source closure and prove artifact determinism."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from prove_lainc_fixed_point import prove


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "scripts" / "build_srclainc.py"


def build(output: Path) -> None:
    result = subprocess.run(
        [sys.executable, str(BUILD), "--output", str(output)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="srclainc-artifact-", dir=ROOT / "build") as directory:
        root = Path(directory)
        first = root / "first.l1"
        second = root / "second.l1"
        build(first)
        build(second)
        report = prove(first, second)

    required = (
        report["canonical_equal"],
        report["extern_equal"],
        report["procedure_labels_equal"],
        not report["header_mismatches"],
        not report["body_mismatches"],
    )
    if not all(required):
        raise SystemExit(f"srclainc artifact is not deterministic: {report}")
    print(
        "PASS deterministic srclainc source-closure artifact "
        f"({report['procedure_count'][0]} procedures)"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"srclainc artifact check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
