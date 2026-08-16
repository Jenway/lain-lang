#!/usr/bin/env python3
"""Run the real LAIN-IR compiler fixed point.

The compiler implementation is ``src/lainir/compiler.l1``.  The C bootstrap
interpreter executes that LAIN-IR source once to produce a native gen1
compiler.  Each later generation is produced by the preceding one, so the
gen2/gen3 equality check covers the compiler's parser, verifier, evaluator
and C emitter instead of merely copying an input bundle.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BOOTSTRAP = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)
COMPILER = ROOT / "src" / "lainir" / "compiler.l1"
HOST = ROOT / "seed" / "src" / "host" / "native_compiler.c"


def run(arguments: list[Path | str], *, env: dict[str, str] | None = None) -> None:
    result = subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(detail or f"command failed with {result.returncode}")


def find_c_compiler() -> list[str]:
    zig = shutil.which("zig")
    if zig:
        return [zig, "cc"]
    for name in ("clang", "cc", "gcc"):
        candidate = shutil.which(name)
        if candidate:
            return [candidate]
    raise RuntimeError("no C compiler is available for the native build")


def compile_native(c_compiler: list[str], source: Path, output: Path, env: dict[str, str]) -> None:
    run(
        [
            *c_compiler,
            "-std=c11",
            str(source),
            str(HOST),
            "-o",
            str(output),
        ],
        env=env,
    )


def main() -> int:
    if not BOOTSTRAP.exists():
        raise RuntimeError(f"missing bootstrap interpreter: {BOOTSTRAP}")
    if not COMPILER.exists():
        raise RuntimeError(f"missing LAIN-IR compiler source: {COMPILER}")
    if not HOST.exists():
        raise RuntimeError(f"missing capability-only native host: {HOST}")

    c_compiler = find_c_compiler()
    with tempfile.TemporaryDirectory(prefix="lainir-self-host-") as temporary:
        work = Path(temporary)
        env = os.environ.copy()
        # Zig otherwise tries to create caches under a restricted user path in
        # the desktop runner.  Keep all generated state inside the workspace.
        env.setdefault("ZIG_LOCAL_CACHE_DIR", str(ROOT / "target" / "zig-cache" / "local"))
        env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ROOT / "target" / "zig-cache" / "global"))

        gen1_c = work / "lainir-c-gen1.c"
        gen2_c = work / "lainir-c-gen2.c"
        gen3_c = work / "lainir-c-gen3.c"
        suffix = ".exe" if os.name == "nt" else ""
        gen1 = work / f"lainir-c-gen1{suffix}"
        gen2 = work / f"lainir-c-gen2{suffix}"

        run([BOOTSTRAP, COMPILER, "lainir_compile_module", gen1_c, COMPILER])
        compile_native(c_compiler, gen1_c, gen1, env)

        run([gen1, "--module", gen2_c, COMPILER], env=env)
        compile_native(c_compiler, gen2_c, gen2, env)

        run([gen2, "--module", gen3_c, COMPILER], env=env)

        if gen2_c.read_bytes() != gen3_c.read_bytes():
            raise RuntimeError("gen2 and gen3 C output differ")

    print("PASS LAIN-IR gen1 -> gen2 -> gen3 C fixed point")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL LAIN-IR self-hosting: {error}", file=sys.stderr)
        raise SystemExit(1)
