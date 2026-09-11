#!/usr/bin/env python3
"""Build a native lainc executable from a generated canonical-L1 compiler."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
SEED = seed_exe("lainir-seed")
CHECK = seed_exe("lainir-print")
BACKEND_L1 = ROOT / "build" / "backend_c_entry.l1"
HOST = ROOT / "seed" / "src" / "host" / "native_lainc.c"
# Sources linked into the final native lainc, alongside the C generated from
# the canonical-L1 compiler.  This closure is intentionally distinct from
# SEED_C_SOURCES in scripts/run_lainir_self_host.py: that one serves the seed
# LAINIR compiler, this one serves lainc.  host/host_io.c is required here
# because native_lainc.c's --run path calls lainir_host_read_file;
# text/emitter.c is absent because nothing in this closure calls it.
IN_PROCESS_SOURCES = (
    ROOT / "seed" / "src" / "core" / "lainir.c",
    ROOT / "seed" / "src" / "core" / "verifier.c",
    ROOT / "seed" / "src" / "text" / "parser.c",
    ROOT / "seed" / "src" / "interpreter" / "interpreter.c",
    ROOT / "seed" / "src" / "interpreter" / "vm_control.c",
    ROOT / "seed" / "src" / "host" / "host_io.c",
)


def run(*args: Path | str, env: dict[str, str] | None = None) -> None:
    result = subprocess.run([str(arg) for arg in args], cwd=ROOT, env=env, text=True)
    if result.returncode:
        raise SystemExit(result.returncode)


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print("usage: build_lainc_native.py <compiler.l1> <lainc.exe> [backend.c]")
        return 2
    compiler_l1 = Path(sys.argv[1])
    executable = Path(sys.argv[2])
    generated_c = Path(sys.argv[3]) if len(sys.argv) == 4 else ROOT / "build" / "lainc-native.c"
    # Never feed an unchecked compiler artifact to the Lain-written backend.
    run(CHECK, compiler_l1, "compiler_compile")
    run(SEED, "interpreter", BACKEND_L1, "main", generated_c, compiler_l1)

    env = os.environ.copy()
    env.setdefault("ZIG_LOCAL_CACHE_DIR", str(ROOT / "build" / "zig-cache"))
    env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ROOT / "build" / "zig-cache-global"))
    # The generated C intentionally keeps the compiler's metadata walks
    # straightforward.  Native consumers need an optimized build: at -O0,
    # archive API expressions spend minutes in those tight scan loops.
    run(
        "zig", "cc", "-std=c11", "-O2", "-DLAIN_NATIVE_LIBRARY_ENTRY",
        "-I", ROOT / "seed" / "include", "-I", ROOT / "seed" / "src" / "host",
        generated_c, HOST, *IN_PROCESS_SOURCES, "-o", executable, env=env
    )
    print(executable)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
