#!/usr/bin/env python3
"""Build the standalone LAIN-IR compiler from the seed interpreter.

The compiler implementation is ``seed/lainir/compiler.l1``. The seed executes
that source once and this script installs the resulting native compiler at
the repository root as ``zig-out/bin/lainir-compiler``.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED_BOOTSTRAP = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)
COMPILER = ROOT / "seed" / "lainir" / "compiler.l1"
COMPILER_PARTS = ROOT / "seed" / "lainir" / "compiler_parts"
HOST = ROOT / "seed" / "src" / "host" / "native_compiler.c"
SEED_C_SOURCES = [
    ROOT / "seed" / "src" / "core" / "lainir.c",
    ROOT / "seed" / "src" / "core" / "verifier.c",
    ROOT / "seed" / "src" / "text" / "parser.c",
    ROOT / "seed" / "src" / "text" / "emitter.c",
    ROOT / "seed" / "src" / "interpreter" / "interpreter.c",
    ROOT / "seed" / "src" / "interpreter" / "eval_source.c",
    ROOT / "seed" / "src" / "interpreter" / "vm_control.c",
]
OUTPUT_DIR = ROOT / "zig-out" / "bin"
BOOTSTRAP = OUTPUT_DIR / ("lainir-seed.exe" if os.name == "nt" else "lainir-seed")
OUTPUT = OUTPUT_DIR / ("lainir-compiler.exe" if os.name == "nt" else "lainir-compiler")


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
            "-I",
            str(ROOT / "seed" / "include"),
            str(source),
            str(HOST),
            *[str(path) for path in SEED_C_SOURCES],
            "-o",
            str(output),
        ],
        env=env,
    )


def main() -> int:
    if not SEED_BOOTSTRAP.exists():
        raise RuntimeError(f"missing bootstrap interpreter: {SEED_BOOTSTRAP}")
    if not COMPILER.exists():
        raise RuntimeError(f"missing LAIN-IR compiler source: {COMPILER}")
    if not HOST.exists():
        raise RuntimeError(f"missing capability-only native host: {HOST}")

    c_compiler = find_c_compiler()
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    # Keep the repository-level bin directory as the only formal artifact
    # location.  Zig's local build directory remains an implementation detail
    # of `zig build` under seed/.
    shutil.copy2(SEED_BOOTSTRAP, BOOTSTRAP)
    with tempfile.TemporaryDirectory(prefix="lainir-self-host-") as temporary:
        work = Path(temporary)
        compiler_input = work / "compiler.l1"
        order = [line.strip() for line in (COMPILER_PARTS / "SOURCE_ORDER").read_text().splitlines() if line.strip()]
        compiler_input.write_bytes(b"".join((COMPILER_PARTS / f"{name}.l1").read_bytes() for name in order))
        if compiler_input.read_bytes() != COMPILER.read_bytes():
            raise RuntimeError("compiler parts do not reconstruct compiler.l1")
        env = os.environ.copy()
        # Zig otherwise tries to create caches under a restricted user path in
        # the desktop runner.  Keep all generated state inside the workspace.
        env.setdefault("ZIG_LOCAL_CACHE_DIR", str(ROOT / "target" / "zig-cache" / "local"))
        env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ROOT / "target" / "zig-cache" / "global"))

        bootstrap_c = work / "bootstrap_stage.c"
        self_host_c = work / "self_host_stage.c"
        self_host_check_c = work / "self_host_check.c"
        suffix = ".exe" if os.name == "nt" else ""
        gen1 = OUTPUT
        self_host = work / f"lainir-compiler-self-host{suffix}"

        OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
        run([BOOTSTRAP, compiler_input, "lainir_compile_module", bootstrap_c, compiler_input])
        compile_native(c_compiler, bootstrap_c, gen1, env)
        # On Windows Zig's linker emits a sibling PDB by default.  The PDB is
        # a transient debug artifact, not part of the standalone compiler
        # contract; keep the repository-level bin directory limited to the
        # two executable products.
        generated_pdb = gen1.with_suffix(".pdb")
        if generated_pdb.exists():
            generated_pdb.unlink()

        run([gen1, "--module", self_host_c, compiler_input], env=env)
        compile_native(c_compiler, self_host_c, self_host, env)

        run([self_host, "--module", self_host_check_c, compiler_input], env=env)

        if self_host_c.read_bytes() != self_host_check_c.read_bytes():
            raise RuntimeError("gen2 and gen3 C output differ")

    print(f"PASS built standalone LAIN-IR compiler: {OUTPUT}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL LAIN-IR self-hosting: {error}", file=sys.stderr)
        raise SystemExit(1)
