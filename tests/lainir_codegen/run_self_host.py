#!/usr/bin/env python3
"""Build the LAIN-IR compiler through its fixed point."""

from __future__ import annotations

import pathlib
import os
import shutil
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
BOOTSTRAP = (
    ROOT
    / "bootstrap"
    / "zig-out"
    / "bin"
    / ("l1bootstrap.exe" if sys.platform == "win32" else "l1bootstrap")
)
COMPILER = ROOT / "src" / "lainir" / "compiler.l1"
HOST = ROOT / "bootstrap" / "src" / "host" / "native_compiler.c"
FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "return_42.l1"
NESTED_EVAL_FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "eval_nested_42.l1"
ARGS_EVAL_FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "eval_call_args.l1"


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env.setdefault("ZIG_LOCAL_CACHE_DIR", str(ROOT / "target" / "zig-cache" / "local"))
    env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ROOT / "target" / "zig-cache" / "global"))
    return subprocess.run(
        command, cwd=ROOT, env=env, text=True, capture_output=True
    )


def require(result: subprocess.CompletedProcess[str], stage: str) -> None:
    if result.returncode:
        raise RuntimeError(
            f"{stage} failed with {result.returncode}\n"
            f"{result.stdout}{result.stderr}"
        )


def main() -> int:
    zig = shutil.which("zig")
    c_compiler = [zig, "cc"] if zig else None
    for name in () if c_compiler is not None else ("clang", "cc", "gcc"):
        found = shutil.which(name)
        if not found:
            continue
        try:
            probe = subprocess.run([found, "--version"], capture_output=True, timeout=5)
        except (OSError, subprocess.SubprocessError):
            continue
        if probe.returncode == 0:
            c_compiler = [found]
            break
    if not BOOTSTRAP.exists() or c_compiler is None:
        print("self-host prerequisites are unavailable", file=sys.stderr)
        return 1

    suffix = ".exe" if sys.platform == "win32" else ""
    with tempfile.TemporaryDirectory(prefix="lainir-self-host-") as temporary:
        work = pathlib.Path(temporary)
        stage1_c = work / "lainir-c.stage1.c"
        stage2_c = work / "lainir-c.stage2.c"
        stage3_c = work / "lainir-c.stage3.c"
        stage1 = work / f"lainir-c.stage1{suffix}"
        stage2 = work / f"lainir-c.stage2{suffix}"

        require(
            run(
                [
                    str(BOOTSTRAP),
                    str(COMPILER),
                    "lainir_compile_module",
                    str(stage1_c),
                    str(COMPILER),
                ]
            ),
            "stage 0 -> stage 1 C",
        )
        require(
            run(
                [
                    *c_compiler,
                    "-std=c11",
                    str(stage1_c),
                    str(HOST),
                    "-o",
                    str(stage1),
                ]
            ),
            "stage 1 native link",
        )

        stage0_fixture = work / "fixture.stage0.c"
        stage1_fixture = work / "fixture.stage1.c"
        require(
            run(
                [
                    str(BOOTSTRAP),
                    str(COMPILER),
                    "lainir_compile",
                    str(stage0_fixture),
                    str(FIXTURE),
                ]
            ),
            "stage 0 fixture compile",
        )
        require(
            run([str(stage1), str(stage1_fixture), str(FIXTURE)]),
            "stage 1 fixture compile",
        )
        if stage0_fixture.read_bytes() != stage1_fixture.read_bytes():
            raise RuntimeError("stage 0 and stage 1 disagree on the fixture")

        stage0_nested = work / "nested.stage0.c"
        stage1_nested = work / "nested.stage1.c"
        require(
            run(
                [
                    str(BOOTSTRAP),
                    str(COMPILER),
                    "lainir_compile",
                    str(stage0_nested),
                    str(NESTED_EVAL_FIXTURE),
                ]
            ),
            "stage 0 nested eval compile",
        )
        require(
            run([str(stage1), str(stage1_nested), str(NESTED_EVAL_FIXTURE)]),
            "stage 1 nested eval compile",
        )
        if stage0_nested.read_bytes() != stage1_nested.read_bytes():
            raise RuntimeError("stage 0 and stage 1 disagree on nested eval")
        if b"return 42;" not in stage0_nested.read_bytes():
            raise RuntimeError("nested eval was not materialized at compile time")

        stage0_args = work / "args.stage0.c"
        stage1_args = work / "args.stage1.c"
        require(
            run(
                [
                    str(BOOTSTRAP),
                    str(COMPILER),
                    "lainir_compile",
                    str(stage0_args),
                    str(ARGS_EVAL_FIXTURE),
                ]
            ),
            "stage 0 constant-argument eval compile",
        )
        require(
            run([str(stage1), str(stage1_args), str(ARGS_EVAL_FIXTURE)]),
            "stage 1 constant-argument eval compile",
        )
        if stage0_args.read_bytes() != stage1_args.read_bytes():
            raise RuntimeError("stage 0 and stage 1 disagree on constant-argument eval")

        require(
            run([str(stage1), "--module", str(stage2_c), str(COMPILER)]),
            "stage 1 -> stage 2 C",
        )
        require(
            run(
                [
                    *c_compiler,
                    "-std=c11",
                    str(stage2_c),
                    str(HOST),
                    "-o",
                    str(stage2),
                ]
            ),
            "stage 2 native link",
        )
        require(
            run([str(stage2), "--module", str(stage3_c), str(COMPILER)]),
            "stage 2 -> stage 3 C",
        )
        if stage2_c.read_bytes() != stage3_c.read_bytes():
            raise RuntimeError("self-hosting did not converge byte-for-byte")

    print(
        "LAIN-IR self-host: stage 1 executed, stage 0/1 agreed, "
        "and stage 2/3 C converged byte-for-byte"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
