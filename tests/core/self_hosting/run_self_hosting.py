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
STAGE3 = OUT / "stage3_compiler.l1"


def tool(name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out/bin" / f"{name}{suffix}"


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def require(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(f"{label}: {detail}")


def compile_single(
    lainc: Path, artifact: Path, source: Path, output: Path
) -> subprocess.CompletedProcess[str]:
    output.unlink(missing_ok=True)
    return run(
        [
            str(lainc),
            "--artifact",
            str(artifact),
            "--emit-l1",
            str(source),
            str(output),
        ]
    )


def require_same_file(left: Path, right: Path, label: str) -> None:
    if left.read_bytes() != right.read_bytes():
        raise RuntimeError(f"{label}: generated LAIN-IR differs")


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
        require(run([sys.executable, str(HERE / "build_stage3_compiler.py")]),
                "build stage3 with stage2")
        require(run([str(l1check), str(STAGE3), "compiler_compile"]),
                "verify stage3")
        require_same_file(STAGE2, STAGE3, "compiler stage2/stage3 fixed point")
        for label, artifact in (("stage2", STAGE2), ("stage3", STAGE3)):
            schema = run([str(l1i), str(artifact), "compiler_api_schema_version"])
            require(schema, f"read {label} schema")
            if schema.stdout.strip() != "1":
                raise RuntimeError(
                    f"{label} schema: expected 1, got {schema.stdout!r}"
                )

        for source in (
            "ast_capability_contract.lain",
            "stable_syntax_handles.lain",
            "generated_syntax_handles.lain",
        ):
            result = run([str(lainc), "--interpret", str(HERE / source), "main"])
            require(result, source)
            if result.stdout.strip() != "42":
                raise RuntimeError(f"{source}: expected 42, got {result.stdout!r}")

        single = HERE / "fixtures/cli_single.lain"
        single_stage2 = OUT / "cli_single_stage2.l1"
        single_stage3 = OUT / "cli_single_stage3.l1"
        require(compile_single(lainc, STAGE2, single, single_stage2),
                "single-file stage2 CLI")
        require(compile_single(lainc, STAGE3, single, single_stage3),
                "single-file stage3 CLI")
        require_same_file(
            single_stage2, single_stage3, "single-file stage2/stage3"
        )
        require(run([str(l1check), str(single_stage3), "main"]),
                "verify single-file output")
        value = run([str(l1i), str(single_stage3), "main"])
        require(value, "execute single-file output")
        if value.stdout.strip() != "42":
            raise RuntimeError(f"single-file output: expected 42, got {value.stdout!r}")

        math = HERE / "fixtures/math_module.lain"
        app = HERE / "fixtures/app_module.lain"
        for label, sources in (
            ("ordered", (math, app)),
            ("reversed", (app, math)),
        ):
            outputs: list[Path] = []
            for stage, artifact in (("stage2", STAGE2), ("stage3", STAGE3)):
                output = OUT / f"cli_workspace_{label}_{stage}.l1"
                output.unlink(missing_ok=True)
                require(
                    run(
                        [
                            str(lainc),
                            "--artifact",
                            str(artifact),
                            "--emit-workspace-l1",
                            str(output),
                            *(str(path) for path in sources),
                        ]
                    ),
                    f"{label} workspace with {stage}",
                )
                outputs.append(output)
            require_same_file(
                outputs[0],
                outputs[1],
                f"{label} workspace stage2/stage3",
            )
            result = run([str(l1i), str(outputs[1]), "app__main"])
            require(result, f"execute {label} workspace")
            if result.stdout.strip() != "42":
                raise RuntimeError(
                    f"{label} workspace: expected 42, got {result.stdout!r}"
                )

        invalid = HERE / "fixtures/cli_invalid.lain"
        diagnostics: list[str] = []
        for label, artifact in (("stage2", STAGE2), ("stage3", STAGE3)):
            invalid_l1 = OUT / f"cli_invalid_{label}.l1"
            rejected = compile_single(lainc, artifact, invalid, invalid_l1)
            if (
                rejected.returncode == 0
                or invalid_l1.exists()
                or "error 2301" not in rejected.stderr
                or "unknown procedure" not in rejected.stderr
            ):
                raise RuntimeError(
                    f"invalid source was not rejected cleanly by {label}: "
                    + (rejected.stderr or rejected.stdout).strip()
                )
            diagnostics.append(rejected.stderr)
        if diagnostics[0] != diagnostics[1]:
            raise RuntimeError("stage2/stage3 diagnostics differ")
    except RuntimeError as error:
        print(f"FAIL self-hosting: {error}")
        return 1

    print(
        "PASS stage1 -> stage2 -> stage3 byte fixed point, syntax "
        "capabilities, stage2/stage3 output and diagnostic equivalence, "
        "ordered/reversed module workspace"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
