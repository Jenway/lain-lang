#!/usr/bin/env python3
"""Compile every supported user-facing example to LAIN-IR."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
EXAMPLES = ROOT / "examples"


def compiler() -> pathlib.Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out" / "bin" / f"lainc{suffix}"


def main() -> int:
    lainc = compiler()
    if not lainc.exists():
        print(f"FAIL compiler missing: {lainc}", file=sys.stderr)
        return 1

    sources = sorted(EXAMPLES.glob("*.lain"))
    if not sources:
        print("FAIL no supported examples found", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="lain-examples-") as tmp:
        out_dir = pathlib.Path(tmp)
        for source in sources:
            output = out_dir / f"{source.stem}.l1"
            result = subprocess.run(
                [str(lainc), "--emit-l1", str(source), str(output)],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            if result.returncode != 0 or not output.exists():
                detail = (result.stderr or result.stdout).strip()
                print(f"FAIL {source.name}: {detail}", file=sys.stderr)
                return 1
            print(f"PASS {source.name}")

    print(f"Examples: {len(sources)} supported sources compiled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

