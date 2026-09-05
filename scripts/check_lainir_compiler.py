#!/usr/bin/env python3
"""Run the short, standalone LAIN-IR compiler regression.

This deliberately exercises only the seed -> compiler -> C path.  It creates
all inputs under a temporary directory and never grows a checked-in fixture
tree.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED_DIR = ROOT / "seed"
COMPILER = ROOT / "zig-out" / "bin" / (
    "lainir-compiler.exe" if os.name == "nt" else "lainir-compiler"
)

SAMPLES = {
    "data": (
        "#data answer(4, 4, \"*\");\n"
        "#proc main() -> #bits<32> {\n"
        "  #let %byte: #bits<8> = #load[#bits<8>](#data_addr(answer))\n"
        "  #return #zext[#bits<32>](%byte)\n"
        "}\n",
        42,
    ),
    "constant": ("#proc main() -> #bits<32> {\n  #return 41\n}\n", 41),
    "arithmetic": (
        "#proc main() -> #bits<32> {\n"
        "  #return #add(#mul(6, 7), 1)\n}\n",
        43,
    ),
    "call": (
        "#proc add1(#bits<32> %x) -> #bits<32> {\n"
        "  #return #add(%x, 1)\n}\n"
        "#proc main() -> #bits<32> {\n"
        "  #return #call add1(41)\n}\n",
        42,
    ),
    "eval": (
        "#proc main() -> #bits<32> {\n"
        "  #let %x: #bits<32> = #eval { #return 40 }\n"
        "  #return #add(%x, 2)\n}\n",
        42,
    ),
    "branch": (
        "#proc main() -> #bits<32> {\n"
        "  #if #eq(1, 1) {\n"
        "    #return 42\n"
        "  } else {\n"
        "    #return 0\n"
        "  }\n"
        "}\n",
        42,
    ),
    "memory": (
        "#proc main() -> #bits<32> {\n"
        "  #let %memory: #addr = #alloca(4)\n"
        "  #let %slot: #addr = #lea(base=%memory, idx=0, scale=1, offset=0)\n"
        "  #let %answer: #bits<32> = 42\n"
        "  #store[#bits<32>] %answer, %slot\n"
        "  #let %loaded: #bits<32> = #load[#bits<32>](%slot)\n"
        "  #return %loaded\n"
        "}\n",
        42,
    ),
    "loop": (
        "#proc main() -> #bits<32> {\n"
        "  #let %i: #bits<32> = 0\n"
        "  #loop {\n"
        "    #if #eq(%i, 3) { #break }\n"
        "    %i = #add(%i, 1)\n"
        "    #continue\n"
        "  }\n"
        "  #return %i\n"
        "}\n",
        3,
    ),
}

MODULE_SAMPLES = {
    "unit_eval": (
        "#proc helper() -> #unit {\n"
        "  #let %done: #unit = #eval { #return }\n"
        "  #return\n"
        "}\n"
    ),
}


def run(command: list[str], *, cwd: Path = ROOT, env: dict[str, str] | None = None) -> None:
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            capture_output=True)
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"{' '.join(command)}: {detail}")


def main() -> int:
    zig = shutil.which("zig")
    if not zig:
        raise RuntimeError("zig is required")
    env = os.environ.copy()
    env.setdefault("ZIG_LOCAL_CACHE_DIR", str(ROOT / "target" / "zig-cache" / "local"))
    env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(ROOT / "target" / "zig-cache" / "global"))

    run([zig, "build"], cwd=SEED_DIR, env=env)
    run([os.fspath(Path(sys.executable)), os.fspath(ROOT / "scripts" / "run_lainir_self_host.py")], env=env)
    if not COMPILER.exists():
        raise RuntimeError(f"missing generated compiler: {COMPILER}")

    with tempfile.TemporaryDirectory(prefix="lainir-check-") as directory:
        work = Path(directory)
        for name, (source, expected) in SAMPLES.items():
            input_path = work / f"{name}.l1"
            c_path = work / f"{name}.c"
            exe_path = work / f"{name}.exe"
            input_path.write_text(source, encoding="utf-8", newline="")
            run([os.fspath(COMPILER), os.fspath(c_path), os.fspath(input_path)], env=env)
            run([zig, "cc", "-std=c11", os.fspath(c_path), "-o", os.fspath(exe_path)], env=env)
            result = subprocess.run([os.fspath(exe_path)], cwd=ROOT, env=env)
            if result.returncode != expected:
                raise RuntimeError(f"{name}: expected {expected}, got {result.returncode}")
            print(f"PASS {name}: {expected}")
        for name, source in MODULE_SAMPLES.items():
            input_path = work / f"{name}.l1"
            c_path = work / f"{name}.c"
            input_path.write_text(source, encoding="utf-8", newline="")
            run([os.fspath(COMPILER), "--module", os.fspath(c_path),
                 os.fspath(input_path)], env=env)
            run([zig, "cc", "-std=c11", "-c", os.fspath(c_path), "-o",
                 os.fspath(work / f"{name}.obj")], env=env)
            print(f"PASS {name}: module C compiles")

        invalid = work / "invalid.l1"
        invalid.write_text("#proc main( -> #bits<32> {\n", encoding="utf-8", newline="")
        rejected = subprocess.run(
            [os.fspath(COMPILER), os.fspath(work / "invalid.c"), os.fspath(invalid)],
            cwd=ROOT, env=env, text=True, capture_output=True,
        )
        if rejected.returncode == 0 or (work / "invalid.c").exists():
            raise RuntimeError("invalid input was accepted or left an artifact")
        print("PASS invalid input: diagnostic and no artifact")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL LAIN-IR compiler check: {error}", file=sys.stderr)
        raise SystemExit(1)
