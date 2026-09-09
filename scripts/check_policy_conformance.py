#!/usr/bin/env python3
"""Compare the bootstrap and formal stdlib policy ABI on fixed inputs."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
CORE = ROOT / "build" / "bootstrap" / "compiler_core.l1"
BOOTSTRAP = ROOT / "build" / "bootstrap" / "stdlib.l1"
FORMAL = ROOT / "build" / "lainir" / "formal_stdlib.l1"
SEED = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)
PROBE = ROOT / "scripts" / "fixtures" / "formal_meta_policy_probe.l1"


def run(
    arguments: list[Path | str], *, artifact: Path | None = None
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    if artifact is not None:
        environment["LAINIR_RUN_ARTIFACT"] = str(artifact)
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
        env=environment,
    )


def main() -> int:
    required = (CORE, BOOTSTRAP, FORMAL, SEED, PROBE)
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        print("policy conformance: missing artifacts: " + ", ".join(missing), file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="lain-policy-conformance-") as directory:
        work = Path(directory)
        outputs: list[str] = []
        for label, stdlib in (("bootstrap", BOOTSTRAP), ("formal", FORMAL)):
            bundle = work / f"{label}.l1"
            output = work / f"{label}.out"
            bundled = run([sys.executable, BUNDLER, "-o", bundle, CORE, stdlib, PROBE])
            if bundled.returncode:
                detail = bundled.stderr.strip() or bundled.stdout.strip()
                raise RuntimeError(f"{label}: bundle failed: {detail}")
            executed = run([SEED, "run", bundle, "main"], artifact=output)
            if executed.returncode:
                detail = executed.stderr.strip() or executed.stdout.strip()
                raise RuntimeError(f"{label}: probe failed: {detail}")
            value = output.read_text(encoding="utf-8")
            if value != "policy=1\n":
                raise RuntimeError(f"{label}: expected policy=1, got {value!r}")
            outputs.append(value)

        if outputs[0] != outputs[1]:
            raise RuntimeError("bootstrap and formal policy outputs differ")
    print("PASS stdlib policy conformance: type/generic/effect/bounds")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL stdlib policy conformance: {error}", file=sys.stderr)
        raise SystemExit(1)
