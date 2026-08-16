#!/usr/bin/env python3
"""Measure whether the frozen bootstrap compiler accepts the full closure."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

from full_closure import ROOT, build_full_closure


BOOTSTRAP = Path(
    os.environ.get(
        "LAIN_BOOTSTRAP_ROOT",
        ROOT / "target" / "bootstrap-racket-worktree",
    )
).resolve()
BIN = BOOTSTRAP / "zig-out" / "bin"
FROZEN = BOOTSTRAP / "seed" / "frozen" / "lainc.l1"
OUT = ROOT / "build/core-self-hosting/full_compiler_stage2.l1"
STAGE3 = ROOT / "build/core-self-hosting/full_compiler_stage3.l1"


def tool(name: str) -> Path:
    return BIN / (name + (".exe" if os.name == "nt" else ""))


def run(arguments: list[Path | str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        check=False,
        capture_output=True,
        text=True,
    )


def main() -> int:
    source, sources = build_full_closure()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    compiled = run(
        [
            tool("l1bootstrap"),
            FROZEN,
            "compiler_compile",
            OUT,
            source,
        ]
    )
    if compiled.returncode != 0:
        sys.stdout.write(compiled.stdout)
        sys.stderr.write(compiled.stderr)
        print(
            f"stage1 compatibility: compile failed for {len(sources)} sources",
            file=sys.stderr,
        )
        return 1

    checked = run([tool("l1check"), OUT, "compiler_compile"])
    if checked.returncode != 0:
        sys.stdout.write(checked.stdout)
        sys.stderr.write(checked.stderr)
        print(
            f"stage1 compatibility: invalid L1 for {len(sources)} sources",
            file=sys.stderr,
        )
        return 1

    rebuilt = run(
        [
            tool("l1bootstrap"),
            OUT,
            "compiler_compile",
            STAGE3,
            source,
        ]
    )
    if rebuilt.returncode != 0:
        sys.stdout.write(rebuilt.stdout)
        sys.stderr.write(rebuilt.stderr)
        print("full closure: stage2 failed to emit stage3", file=sys.stderr)
        return 1

    checked_stage3 = run([tool("l1check"), STAGE3, "compiler_compile"])
    if checked_stage3.returncode != 0:
        sys.stdout.write(checked_stage3.stdout)
        sys.stderr.write(checked_stage3.stderr)
        print("full closure: stage3 is invalid L1", file=sys.stderr)
        return 1

    if OUT.read_bytes() != STAGE3.read_bytes():
        print("full closure: stage2/stage3 byte mismatch", file=sys.stderr)
        return 1

    print(
        f"full compiler fixed point: PASS "
        f"({len(sources)} sources, {OUT.stat().st_size} bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
