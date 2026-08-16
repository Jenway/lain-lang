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
    / "seed"
    / "zig-out"
    / "bin"
    / ("lainir-seed.exe" if sys.platform == "win32" else "lainir-seed")
)
COMPILER = ROOT / "src" / "lainir" / "compiler.l1"
HOST = ROOT / "seed" / "src" / "host" / "native_compiler.c"
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


def require(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode:
        raise RuntimeError(
            f"{label} failed with {result.returncode}\n"
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
        gen1_c = work / "lainir-c-gen1.c"
        gen2_c = work / "lainir-c-gen2.c"
        gen3_c = work / "lainir-c-gen3.c"
        gen1 = work / f"lainir-c-gen1{suffix}"
        gen2 = work / f"lainir-c-gen2{suffix}"

        require(
            run(
                [
                    str(BOOTSTRAP),
                    str(COMPILER),
                    "lainir_compile_module",
                    str(gen1_c),
                    str(COMPILER),
                ]
            ),
            "seed -> gen1 C",
        )
        require(
            run(
                [
                    *c_compiler,
                    "-std=c11",
                    str(gen1_c),
                    str(HOST),
                    "-o",
                    str(gen1),
                ]
            ),
            "gen1 native link",
        )

        seed_fixture = work / "fixture.seed.c"
        gen1_fixture = work / "fixture.gen1.c"
        require(
            run(
                [
                    str(BOOTSTRAP),
                    str(COMPILER),
                    "lainir_compile",
                    str(seed_fixture),
                    str(FIXTURE),
                ]
            ),
            "seed fixture compile",
        )
        require(
            run([str(gen1), str(gen1_fixture), str(FIXTURE)]),
            "gen1 fixture compile",
        )
        if seed_fixture.read_bytes() != gen1_fixture.read_bytes():
            raise RuntimeError("seed and gen1 disagree on the fixture")

        seed_nested = work / "nested.seed.c"
        gen1_nested = work / "nested.gen1.c"
        require(
            run(
                [
                    str(BOOTSTRAP),
                    str(COMPILER),
                    "lainir_compile",
                    str(seed_nested),
                    str(NESTED_EVAL_FIXTURE),
                ]
            ),
            "seed nested eval compile",
        )
        require(
            run([str(gen1), str(gen1_nested), str(NESTED_EVAL_FIXTURE)]),
            "gen1 nested eval compile",
        )
        if seed_nested.read_bytes() != gen1_nested.read_bytes():
            raise RuntimeError("seed and gen1 disagree on nested eval")
        if b"return 42;" not in seed_nested.read_bytes():
            raise RuntimeError("nested eval was not materialized at compile time")

        seed_args = work / "args.seed.c"
        gen1_args = work / "args.gen1.c"
        require(
            run(
                [
                    str(BOOTSTRAP),
                    str(COMPILER),
                    "lainir_compile",
                    str(seed_args),
                    str(ARGS_EVAL_FIXTURE),
                ]
            ),
            "seed constant-argument eval compile",
        )
        require(
            run([str(gen1), str(gen1_args), str(ARGS_EVAL_FIXTURE)]),
            "gen1 constant-argument eval compile",
        )
        if seed_args.read_bytes() != gen1_args.read_bytes():
            raise RuntimeError("seed and gen1 disagree on constant-argument eval")

        require(
            run([str(gen1), "--module", str(gen2_c), str(COMPILER)]),
            "gen1 -> gen2 C",
        )
        require(
            run(
                [
                    *c_compiler,
                    "-std=c11",
                    str(gen2_c),
                    str(HOST),
                    "-o",
                    str(gen2),
                ]
            ),
            "gen2 native link",
        )
        require(
            run([str(gen2), "--module", str(gen3_c), str(COMPILER)]),
            "gen2 -> gen3 C",
        )
        if gen2_c.read_bytes() != gen3_c.read_bytes():
            raise RuntimeError("self-hosting did not converge byte-for-byte")

    print(
        "LAIN-IR self-host: gen1 executed, seed/gen1 agreed, "
        "and gen2/gen3 C converged byte-for-byte"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
