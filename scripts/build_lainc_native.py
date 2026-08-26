#!/usr/bin/env python3
"""Build a native lainc executable from a generated canonical-L1 compiler."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)
BACKEND_L1 = ROOT / "build" / "backend_c_entry.l1"
HOST = ROOT / "seed" / "src" / "host" / "native_lainc.c"


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
    run(SEED, "interpreter", BACKEND_L1, "main", generated_c, compiler_l1)

    env = os.environ.copy()
    env.setdefault("ZIG_LOCAL_CACHE_DIR", str(ROOT / "target" / "zig-cache" / "local"))
    env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ROOT / "target" / "zig-cache" / "global"))
    # The generated C intentionally keeps the compiler's metadata walks
    # straightforward.  Native consumers need an optimized build: at -O0,
    # archive API expressions spend minutes in those tight scan loops.
    run(
        "zig", "cc", "-std=c11", "-O2", "-DLAIN_NATIVE_LIBRARY_ENTRY",
        generated_c, HOST, "-o", executable, env=env
    )
    print(executable)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
