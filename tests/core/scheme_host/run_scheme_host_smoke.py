#!/usr/bin/env python3
"""Run the Scheme host contract smoke against installed interpreters."""

from __future__ import annotations

import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SMOKE = ROOT / "tests" / "core" / "scheme_host" / "contract_smoke.scm"


@dataclass(frozen=True)
class Candidate:
    name: str
    command: tuple[str, ...]


CANDIDATES = (
    Candidate("gauche", ("gosh", str(SMOKE))),
    Candidate("chez", ("scheme", "--script", str(SMOKE))),
    Candidate("chez-script", ("scheme-script", str(SMOKE))),
    Candidate("gambit", ("gsi", str(SMOKE))),
    Candidate("chicken", ("csi", "-s", str(SMOKE))),
)


def main() -> int:
    any_found = False
    any_passed = False

    for candidate in CANDIDATES:
        exe = candidate.command[0]
        resolved = shutil.which(exe)
        if resolved is None:
            print(f"SKIP {candidate.name}: {exe} not found")
            continue

        any_found = True
        proc = subprocess.run(
            candidate.command,
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        output = (proc.stdout + proc.stderr).strip()
        if proc.returncode == 0:
            any_passed = True
            print(f"PASS {candidate.name}: {resolved}")
        else:
            print(f"FAIL {candidate.name}: {resolved}")
        if output:
            print(output)

    if not any_found:
        print("No candidate Scheme interpreter found on PATH.")
        return 2
    return 0 if any_passed else 1


if __name__ == "__main__":
    sys.exit(main())
