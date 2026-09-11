#!/usr/bin/env python3
"""Build src/lainc with an explicitly selected LAINIR capability provider."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from lainc_sources import composed_compiler_sources
from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
RUN_COMPILER = ROOT / "scripts" / "run_lain_compiler.py"
DEFAULT_OUTPUT = ROOT / "build" / "lainir" / "srclainc.l1"
PRINT = seed_exe("lainir-print")


def verify_artifact(output: Path) -> None:
    """Reject a compiler diagnostic saved as a nominal output artifact."""

    if not output.is_file():
        raise RuntimeError(f"compiler did not write {output}")
    text = output.read_text(encoding="utf-8")
    match = re.search(r"^#proc\s+([^\s(]+)\(", text, re.MULTILINE)
    if not match:
        detail = text.strip().splitlines()[0] if text.strip() else "empty output"
        raise RuntimeError(f"src/lainc compilation did not produce LAINIR: {detail}")
    if not PRINT.is_file():
        raise RuntimeError(f"missing LAINIR verifier: {PRINT}")
    verified = subprocess.run(
        [str(PRINT), str(output), match.group(1)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if verified.returncode:
        raise RuntimeError(
            "src/lainc artifact verification failed: "
            + (verified.stderr.strip() or verified.stdout.strip())
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    output = args.output if args.output.is_absolute() else ROOT / args.output
    output.parent.mkdir(parents=True, exist_ok=True)

    sources = composed_compiler_sources(ROOT)
    command = [
        sys.executable,
        str(RUN_COMPILER),
        "--library",
        "-o",
        str(output),
        *(str(path.relative_to(ROOT)) for path in sources),
    ]
    result = subprocess.run(command, cwd=ROOT, text=True)
    if result.returncode:
        return result.returncode
    try:
        verify_artifact(output)
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 1
    try:
        display_path = output.relative_to(ROOT)
    except ValueError:
        display_path = output
    print(display_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
