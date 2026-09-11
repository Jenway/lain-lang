#!/usr/bin/env python3
"""Run the seed-facing Artifact/Procedure/arguments/eval contract."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
SEED = seed_exe("lainir-seed")
PROBE = ROOT / "scripts" / "fixtures" / "bootstrap_vm_api_probe.l1"
EMPTY = ROOT / "scripts" / "fixtures" / "empty_source.lain"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-bootstrap-vm-api-") as directory:
        output = Path(directory) / "unused.l1"
        result = subprocess.run(
            [
                str(SEED),
                "interpreter",
                str(PROBE),
                "bootstrap_vm_api_probe",
                str(output),
                str(EMPTY),
                str(EMPTY),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if result.returncode:
            print(result.stderr or result.stdout)
            return result.returncode
    print("PASS bootstrap LAINVM Artifact/Procedure/arguments/eval API")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
