#!/usr/bin/env python3
"""Run the repeatable baseline for the lainc -> LAINIR API migration."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHECKS = (
    ("source boundary", "check_lainc_lainir_api.py", "--final"),
    ("provider behavior", "check_lainir_api_behavior.py"),
    ("source-closure determinism", "check_srclainc_artifact.py"),
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
