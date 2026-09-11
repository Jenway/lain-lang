#!/usr/bin/env python3
"""Build the Zig seed compiler into the canonical ``build/`` tree.

This is the only entry point for building ``seed/``.  It redirects Zig's
caches and install prefix out of the source tree so that ``seed/`` keeps only
sources: the installed binaries land in ``build/seed/bin`` and all cache
state lives under ``build/zig-cache``.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SEED_DIR = ROOT / "seed"
LOCAL_CACHE = ROOT / "build" / "zig-cache"
GLOBAL_CACHE = ROOT / "build" / "zig-cache-global"
PREFIX = ROOT / "build" / "seed"

OPTIMIZE_CHOICES = ("Debug", "ReleaseSafe", "ReleaseFast", "ReleaseSmall")


def build(optimize: str | None = None) -> int:
    env = os.environ.copy()
    env["ZIG_LOCAL_CACHE_DIR"] = str(LOCAL_CACHE)
    env["ZIG_GLOBAL_CACHE_DIR"] = str(GLOBAL_CACHE)
    command = ["zig", "build", "--prefix", str(PREFIX)]
    if optimize:
        command += ["-Doptimize=" + optimize]
    completed = subprocess.run(command, cwd=SEED_DIR, env=env)
    if completed.returncode == 0:
        print(f"installed seed binaries in {PREFIX / 'bin'}")
    return completed.returncode


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--optimize",
        choices=OPTIMIZE_CHOICES,
        default=None,
        help="Zig optimization mode forwarded as -Doptimize",
    )
    args = parser.parse_args(argv)
    return build(args.optimize)


if __name__ == "__main__":
    raise SystemExit(main())
