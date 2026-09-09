#!/usr/bin/env python3
"""Verify the physical #data and #alloca safety traps."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "seed" / "zig-out" / ("bin/lainir-seed.exe" if os.name == "nt" else "bin/lainir-seed")

CASES = {
    "readonly_data": (
        '#data answer(4, 4, "*");\n'
        "#proc main() -> #bits<32> {\n"
        "  #store[#bits<32>] 7, #data_addr(answer)\n"
        "  #return 0\n"
        "}\n",
        "store to read-only #data",
    ),
    "alloca_escape": (
        "#proc leak() -> #addr {\n"
        "  #let %memory: #addr = #alloca(8)\n"
        "  #return %memory\n"
        "}\n"
        "#proc main() -> #bits<32> {\n"
        "  #let %escaped: #addr = #call leak()\n"
        "  #return 0\n"
        "}\n",
        "#alloca address escaped procedure activation",
    ),
    "alloca_bounds": (
        "#proc main() -> #bits<32> {\n"
        "  #let %memory: #addr = #alloca(4)\n"
        "  #let %past: #addr = #lea(base=%memory, idx=0, scale=1, offset=4)\n"
        "  #let %value: #bits<32> = #load[#bits<32>](%past)\n"
        "  #return %value\n"
        "}\n",
        "memory access out of bounds",
    ),
}


def main() -> int:
    if not SEED.is_file():
        raise SystemExit("physical safety check requires a built seed")
    with tempfile.TemporaryDirectory(prefix="lainir-physical-safety-") as directory:
        root = Path(directory)
        for name, (source, expected) in CASES.items():
            path = root / f"{name}.l1"
            path.write_text(source, encoding="utf-8", newline="\n")
            result = subprocess.run(
                [str(SEED), "run", str(path), "main"],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            detail = result.stderr + result.stdout
            if result.returncode == 0 or expected not in detail:
                raise SystemExit(
                    f"{name}: expected trap {expected!r}, got rc={result.returncode}: {detail}"
                )
            print(f"PASS {name}: {expected}")
    print("PASS LAINIR physical safety traps")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
