#!/usr/bin/env python3
"""Guard the bootstrap compiler's program entry contract.

`python scripts/run_lain_compiler.py -o OUT SOURCE` (without `--library`)
compiles a complete program with a `main` entry and is the path used to build
and run a real executable.  It went unnoticed that this mode was entirely
broken -- every program failed with 5112 -- because no check exercised it and
every script drives `--library` instead.

The entry contract is that `main` returns a 32-bit value.  This check pins the
contract semantically: any 32-bit spelling is accepted and actually runs, while
a different width is still refused.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / "scripts" / "run_lain_compiler.py"
SEED = seed_exe("lainir-seed")

# (label, main body source, expected stdout) -- accepted entry widths.  The
# spellings differ only in how the same 32-bit width is written.
ACCEPTED = (
    (
        "signed 32-bit entry",
        "let main = std::func() -> i32 {\n    return 42;\n};\n",
        "42",
    ),
    (
        "unsigned 32-bit entry",
        "let main = std::func() -> u32 {\n    return 7;\n};\n",
        "7",
    ),
)

# (label, main body source) -- entry widths the contract must refuse.
REFUSED = (
    ("64-bit entry", "let main = std::func() -> i64 {\n    return 42;\n};\n"),
    ("8-bit entry", "let main = std::func() -> i8 {\n    return 42;\n};\n"),
)

REFUSAL_STATUS = 5112


def run(arguments: list[Path | str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def main() -> int:
    if not COMPILER.is_file() or not SEED.is_file():
        print(
            "program entry: run_lain_compiler.py or the seed is missing",
            file=sys.stderr,
        )
        return 2
    with tempfile.TemporaryDirectory(prefix="lain-program-entry-") as raw:
        work = Path(raw)
        for index, (label, source, expected) in enumerate(ACCEPTED):
            fixture = work / f"accepted_{index}.lain"
            artifact = work / f"accepted_{index}.l1"
            fixture.write_text(source, encoding="utf-8", newline="\n")
            compiled = run(
                [sys.executable, str(COMPILER), "-o", str(artifact), str(fixture)]
            )
            if compiled.returncode:
                print(
                    f"{label}: program mode refused a valid entry: "
                    f"{compiled.stdout + compiled.stderr}".strip(),
                    file=sys.stderr,
                )
                return 1
            executed = run([str(SEED), "run", str(artifact), "main"])
            actual = executed.stdout.strip()
            if executed.returncode or actual != expected:
                print(
                    f"{label}: expected {expected!r}, got "
                    f"code={executed.returncode}, stdout={actual!r}",
                    file=sys.stderr,
                )
                return 1
            print(f"PASS {label}: program compiled and ran, {actual}")

        for index, (label, source) in enumerate(REFUSED):
            fixture = work / f"refused_{index}.lain"
            artifact = work / f"refused_{index}.l1"
            fixture.write_text(source, encoding="utf-8", newline="\n")
            compiled = run(
                [sys.executable, str(COMPILER), "-o", str(artifact), str(fixture)]
            )
            detail = compiled.stdout + compiled.stderr
            if compiled.returncode == 0:
                print(
                    f"{label}: a non-32-bit entry was accepted",
                    file=sys.stderr,
                )
                return 1
            if str(REFUSAL_STATUS) not in detail:
                print(
                    f"{label}: expected status {REFUSAL_STATUS}, got: "
                    f"{detail.strip()}",
                    file=sys.stderr,
                )
                return 1
            print(f"PASS {label}: refused with {REFUSAL_STATUS}")
    print("PASS program entry contract: 32-bit main compiles and runs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
