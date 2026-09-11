#!/usr/bin/env python3
"""Compile and run the default Provider(Memory) through the public API."""

from __future__ import annotations

import re
import subprocess
import tempfile
from pathlib import Path

import check_stdlib_conformance as conformance
from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / "build" / "bootstrap" / "lainc.l1"
FIXTURE = ROOT / "scripts" / "fixtures" / "lainir_provider_smoke.lain"
SEED = seed_exe("lainir-seed")
API_SOURCES = (
    ROOT / "src" / "lainir" / "api_contract.lain",
    ROOT / "src" / "lainir" / "api" / "l1_ir.lain",
    ROOT / "src" / "lainir" / "api" / "l1_unit_builder.lain",
    ROOT / "src" / "lainir" / "api" / "l1_verifier.lain",
    ROOT / "src" / "lainir" / "api" / "l1_printer.lain",
    ROOT / "src" / "lainir" / "api" / "default_provider.lain",
)


def main() -> int:
    required = (COMPILER, FIXTURE, SEED, *API_SOURCES)
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        print("provider smoke: missing files: " + ", ".join(missing))
        return 2

    sources = tuple(sorted((ROOT / "std").rglob("*.lain"))) + API_SOURCES + (FIXTURE,)
    with tempfile.TemporaryDirectory(prefix="lainir-provider-smoke-") as directory:
        artifact = Path(directory) / "provider_smoke.l1"
        result = conformance.compile_sources(COMPILER, sources, artifact)
        if result.returncode:
            print(result.stdout + result.stderr)
            return result.returncode
        match = re.findall(r"f\d+_\d+_\d+_probe", artifact.read_text())
        if not match:
            print("provider smoke: probe procedure missing")
            return 1
        run = subprocess.run(
            [str(SEED), "run", str(artifact), match[-1]],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if run.returncode or run.stdout.strip() != "1":
            print(run.stdout + run.stderr)
            return run.returncode or 1
    print("PASS Provider(Memory) artifact verifier and run")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
