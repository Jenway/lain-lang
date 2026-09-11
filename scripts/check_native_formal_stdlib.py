#!/usr/bin/env python3
"""Run the formal stdlib lowering slice through a native compiler."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "scripts" / "fixtures"
SEED = seed_exe("lainir-seed")
SUCCESS = (
    "formal_constant_return.lain",
    "formal_if_true_return.lain",
    "formal_if_false_return.lain",
    "formal_if_eq_return.lain",
    "formal_if_ne_return.lain",
)


def compile_fixture(compiler: Path, fixture: str, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(compiler), "-o", str(output), str(FIXTURES / fixture)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_native_formal_stdlib.py <formal-lainc.exe>", file=sys.stderr)
        return 2
    compiler = Path(sys.argv[1])
    if not compiler.is_absolute():
        compiler = ROOT / compiler
    if not compiler.exists() or not SEED.exists():
        print("missing formal compiler or seed interpreter", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="native-formal-stdlib-") as directory:
        root = Path(directory)
        for fixture in SUCCESS:
            output = root / f"{fixture}.l1"
            result = compile_fixture(compiler, fixture, output)
            if result.returncode != 0 or not output.exists() or b"\x00" in output.read_bytes():
                print(f"FAIL {fixture}: compile exit={result.returncode}", file=sys.stderr)
                return 1
            executed = subprocess.run(
                [str(SEED), "run", str(output), "main"],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            if executed.returncode != 0 or executed.stdout.strip() != "42":
                print(f"FAIL {fixture}: run={executed.stdout.strip()!r}", file=sys.stderr)
                return 1

        unsupported = root / "formal_if_without_else.l1"
        result = compile_fixture(compiler, "formal_if_without_else.lain", unsupported)
        if result.returncode == 0 or unsupported.exists() or unsupported.with_suffix(unsupported.suffix + ".tmp").exists():
            print("FAIL unsupported formal lowering was not atomic", file=sys.stderr)
            return 1

    print(f"PASS native formal stdlib ({len(SUCCESS)} success, 1 unsupported atomic failure)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
