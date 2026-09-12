#!/usr/bin/env python3
"""Run the seed-facing Artifact/Procedure/arguments/eval contract.

Also covers the in-memory artifact capture sink, which lets a compiler stage
obtain generated LAINIR without a filesystem round trip.
"""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
SEED = seed_exe("lainir-seed")
FIXTURES = ROOT / "scripts" / "fixtures"
PROBE = FIXTURES / "bootstrap_vm_api_probe.l1"
CAPTURE_PROBE = FIXTURES / "bootstrap_capture_probe.l1"
EMPTY = FIXTURES / "empty_source.lain"


def run_probe(probe: Path, entry: str, directory: Path) -> int:
    result = subprocess.run(
        [
            str(SEED),
            "interpreter",
            str(probe),
            entry,
            str(directory / "unused.l1"),
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
    return 0


def main() -> int:
    required = (PROBE, CAPTURE_PROBE, EMPTY)
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        print("bootstrap vm api: missing probes: " + ", ".join(missing))
        return 2

    with tempfile.TemporaryDirectory(prefix="lain-bootstrap-vm-api-") as raw:
        directory = Path(raw)
        status = run_probe(PROBE, "bootstrap_vm_api_probe", directory)
        if status:
            return status
        status = run_probe(
            CAPTURE_PROBE, "bootstrap_capture_probe", directory
        )
        if status:
            return status
    print("PASS bootstrap LAINVM Artifact/Procedure/arguments/eval API")
    print("PASS in-memory artifact capture sink")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
