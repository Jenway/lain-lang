#!/usr/bin/env python3
"""Verify the current Lain self-hosting boundary without milestone probes."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
OUT = ROOT / "build/core-self-hosting"
META = OUT / "meta_compiler.l1"
STAGE2 = OUT / "stage2_compiler.l1"


def tool(name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out/bin" / f"{name}{suffix}"


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def require(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(f"{label}: {detail}")


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    lainc = tool("lainc")
    l1check = tool("l1check")
    l1i = tool("l1i")

    try:
        require(run([sys.executable, str(HERE / "build_meta_artifact.py")]),
                "build stage1")
        require(run([str(l1check), str(META), "compiler_compile"]),
                "verify stage1")
        require(run([sys.executable, str(HERE / "build_self_hosted_compiler.py")]),
                "build stage2")
        require(run([str(l1check), str(STAGE2), "compiler_compile"]),
                "verify stage2")
        schema = run([str(l1i), str(STAGE2), "compiler_api_schema_version"])
        require(schema, "read stage2 schema")
        if schema.stdout.strip() != "1":
            raise RuntimeError(f"stage2 schema: expected 1, got {schema.stdout!r}")

        for source in ("ast_capability_contract.lain", "stable_syntax_handles.lain"):
            result = run([str(lainc), "--interpret", str(HERE / source), "main"])
            require(result, source)
            if result.stdout.strip() != "42":
                raise RuntimeError(f"{source}: expected 42, got {result.stdout!r}")

        single = HERE / "fixtures/cli_single.lain"
        single_l1 = OUT / "cli_single.l1"
        require(run([str(lainc), "--artifact", str(STAGE2),
                     "--emit-l1", str(single), str(single_l1)]),
                "single-file stage2 CLI")
        require(run([str(l1check), str(single_l1), "main"]),
                "verify single-file output")
        value = run([str(l1i), str(single_l1), "main"])
        require(value, "execute single-file output")
        if value.stdout.strip() != "42":
            raise RuntimeError(f"single-file output: expected 42, got {value.stdout!r}")

        math = HERE / "fixtures/math_module.lain"
        app = HERE / "fixtures/app_module.lain"
        for label, sources in (
            ("ordered", (math, app)),
            ("reversed", (app, math)),
        ):
            output = OUT / f"cli_workspace_{label}.l1"
            require(run([str(lainc), "--artifact", str(STAGE2),
                         "--emit-workspace-l1", str(output),
                         *(str(path) for path in sources)]),
                    f"{label} workspace")
            result = run([str(l1i), str(output), "app__main"])
            require(result, f"execute {label} workspace")
            if result.stdout.strip() != "42":
                raise RuntimeError(
                    f"{label} workspace: expected 42, got {result.stdout!r}"
                )

        invalid = HERE / "fixtures/cli_invalid.lain"
        invalid_l1 = OUT / "cli_invalid.l1"
        invalid_l1.unlink(missing_ok=True)
        rejected = run([str(lainc), "--artifact", str(STAGE2),
                        "--emit-l1", str(invalid), str(invalid_l1)])
        if (rejected.returncode == 0 or invalid_l1.exists()
                or "error 2301" not in rejected.stderr
                or "unknown procedure" not in rejected.stderr):
            raise RuntimeError(
                "invalid source was not rejected cleanly: "
                + (rejected.stderr or rejected.stdout).strip()
            )
    except RuntimeError as error:
        print(f"FAIL self-hosting: {error}")
        return 1

    print(
        "PASS stage1 -> stage2 self-hosting, syntax capabilities, "
        "single-file CLI, ordered/reversed module workspace, and diagnostics"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
