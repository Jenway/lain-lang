#!/usr/bin/env python3
"""Execute a Lain-written compiler component through the LAIN-IR interpreter."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
COMPONENT = ROOT / "tests/core/bootstrap_execution/const_lowering_component.lain"
OUT_DIR = ROOT / "build/core-bootstrap-execution"
OUT_L1 = OUT_DIR / "build_const_return_fn.l1"
L1I = OUT_DIR / ("l1i.exe" if os.name == "nt" else "l1i")
COMPAT = ROOT / "tests/core/bootstrap_execution/msvc_posix_compat.h"


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True)


def compiler_path() -> Path:
    names = ("lainc.exe", "lainc")
    for directory in (ROOT / "zig-out" / "bin", ROOT / "src" / "compiler"):
        for name in names:
            candidate = directory / name
            if candidate.exists():
                return candidate
    return ROOT / "zig-out" / "bin" / names[0]


def build_interpreter() -> tuple[bool, str]:
    installed = ROOT / "zig-out" / "bin" / ("l1i.exe" if os.name == "nt" else "l1i")
    if installed.exists():
        shutil.copy2(installed, L1I)
        return True, ""
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
    if cc is None:
        return False, "C compiler unavailable (tried clang, gcc, cc)"
    command = [
        cc,
        "-Isrc",
        "-std=c11",
        "-include",
        str(COMPAT),
        "src/lainir/lain_ir_interp_main.c",
        "src/lainir/lain_ir_parser.c",
        "src/lainir/lainir_core.c",
        "src/lainir/interpreter.c",
        "-o",
        str(L1I),
    ]
    result = run(command)
    if result.returncode != 0:
        return False, (result.stderr or result.stdout).strip()
    return True, ""


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    compiler = compiler_path()
    if not compiler.exists():
        print(f"FAIL compiler missing: {compiler}")
        return 1

    ok, detail = build_interpreter()
    if not ok:
        print(f"FAIL building LAIN-IR interpreter: {detail}")
        return 1

    emitted = run([str(compiler), "--emit-l1", str(COMPONENT), str(OUT_L1)])
    if emitted.returncode != 0:
        print(f"FAIL compiling Lain component: {(emitted.stderr or emitted.stdout).strip()}")
        return 1

    executed = run([str(L1I), str(OUT_L1), "main", "42"])
    if executed.returncode != 0:
        print(f"FAIL executing component: {(executed.stderr or executed.stdout).strip()}")
        return 1
    actual = executed.stdout.strip()
    if actual != "42":
        print(f"FAIL expected 42, got {actual!r}")
        return 1

    print("PASS Lain constant-lowering component -> emitted LAIN-IR -> interpreter result 42")
    return 0


if __name__ == "__main__":
    sys.exit(main())
