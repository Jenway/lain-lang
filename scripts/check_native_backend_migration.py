#!/usr/bin/env python3
"""Run the native backend migration gate on a multi-procedure fixture."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUN_BACKEND = ROOT / "scripts" / "run_lain_backend.py"
FIXTURE = ROOT / "scripts" / "fixtures" / "backend_constant_return.l1"
SEED_SOURCES = (
    ROOT / "seed" / "src" / "core" / "lainir.c",
    ROOT / "seed" / "src" / "core" / "verifier.c",
    ROOT / "seed" / "src" / "text" / "parser.c",
    ROOT / "seed" / "src" / "interpreter" / "interpreter.c",
    ROOT / "seed" / "src" / "interpreter" / "vm_control.c",
    ROOT / "seed" / "src" / "host" / "host_io.c",
)
CAPABILITIES = {
    "backend.allocate",
    "backend.artifact_begin",
    "backend.artifact_finish",
    "backend.artifact_write_byte",
    "backend.copy_bytes",
    "backend.source_count",
    "backend.source_data",
    "backend.source_length",
}


def run(*args: Path | str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [str(arg) for arg in args],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"command failed ({result.returncode}): {detail}")
    return result


def main() -> int:
    if not FIXTURE.is_file():
        raise RuntimeError(f"missing backend fixture: {FIXTURE}")
    with tempfile.TemporaryDirectory(prefix="lain-backend-gate-", dir=ROOT / "build") as temp:
        work = Path(temp)
        c_output = work / "backend.c"
        backend_l1 = work / "backend.l1"
        manifest = c_output.with_suffix(".c.manifest.json")
        run(
            sys.executable,
            RUN_BACKEND,
            FIXTURE,
            "-o",
            c_output,
            "--backend-l1",
            backend_l1,
            "--manifest",
            manifest,
        )
        payload = json.loads(manifest.read_text(encoding="utf-8"))
        if set(payload.get("capabilities", ())) != CAPABILITIES:
            raise RuntimeError("backend manifest does not expose exactly ABI v1 capabilities")
        if not any("backend.allocate" == item.get("symbol") for item in payload["externs"]):
            raise RuntimeError("backend manifest has no logical backend extern")

        env = os.environ.copy()
        env.setdefault("ZIG_LOCAL_CACHE_DIR", str(ROOT / "build" / "zig-cache-local"))
        env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ROOT / "build" / "zig-cache-global"))
        executable = work / ("backend.exe" if os.name == "nt" else "backend")
        run(
            "zig",
            "cc",
            "-std=c11",
            "-O2",
            "-I",
            ROOT / "seed" / "include",
            "-I",
            ROOT / "seed" / "src" / "host",
            c_output,
            ROOT / "seed" / "src" / "host" / "native_lainc.c",
            *SEED_SOURCES,
            "-o",
            executable,
            env=env,
        )
        executed = subprocess.run(
            [str(executable), "-o", work / "result.l1", FIXTURE],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if executed.returncode != 42:
            detail = executed.stderr.strip() or executed.stdout.strip()
            raise RuntimeError(
                f"native backend fixture returned {executed.returncode}: {detail}"
            )
    print("PASS native backend migration gate")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"FAIL native backend migration gate: {error}", file=sys.stderr)
        raise SystemExit(1)
