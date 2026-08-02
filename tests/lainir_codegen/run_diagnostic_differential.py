#!/usr/bin/env python3
"""Check that both front ends reject shared failures in the same phase."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SUFFIX = ".exe" if sys.platform == "win32" else ""
BIN = ROOT / "bootstrap" / "zig-out" / "bin"
CHECKER = BIN / f"l1check{SUFFIX}"
BOOTSTRAP = BIN / f"l1bootstrap{SUFFIX}"
COMPILER = ROOT / "src" / "lainir" / "compiler.l1"

CASES = {
    "malformed-arrow": (
        "parse",
        "#proc main() - #bits<32> { #return 0 }\n",
    ),
    "missing-brace": (
        "parse",
        "#proc main() -> #bits<32> { #return 0\n",
    ),
    "unknown-call": (
        "verify",
        "#proc main() -> #bits<32> {\n"
        "  #return #call missing()\n"
        "}\n",
    ),
    "duplicate-procedure": (
        "verify",
        "#proc main() -> #bits<32> { #return 0 }\n"
        "#proc main() -> #bits<32> { #return 1 }\n",
    ),
    "return-type": (
        "verify",
        "#proc bad(#bits<64> %x) -> #bits<32> { #return %x }\n"
        "#proc main() -> #bits<32> { #return 0 }\n",
    ),
    "store-destination": (
        "verify",
        "#proc main() -> #bits<32> { #let %x: #bits<32> = 1 "
        "#store[#bits<32>] %x, %x #return 0 }\n",
    ),
}


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lainir-diagnostic-diff-") as directory:
        work = pathlib.Path(directory)
        for name, (phase, source) in CASES.items():
            source_path = work / f"{name}.l1"
            debug_path = work / f"{name}.txt"
            source_path.write_text(source, encoding="utf-8", newline="\n")

            reference = run([str(CHECKER), str(source_path), "main"])
            debug = run(
                [
                    str(BOOTSTRAP),
                    str(COMPILER),
                    "lainir_verify_debug",
                    str(debug_path),
                    str(source_path),
                ]
            )
            compiled = run(
                [
                    str(BOOTSTRAP),
                    str(COMPILER),
                    "lainir_compile",
                    str(work / f"{name}.c"),
                    str(source_path),
                ]
            )
            actual_reference_phase = (
                "parse" if reference.stderr.startswith("parse[") else "verify"
            )
            debug_text = (
                debug_path.read_text(encoding="utf-8") if debug_path.exists() else ""
            )
            actual_lainir_phase = (
                "parse"
                if debug_text == "parse"
                else "verify"
                if compiled.returncode != 0
                else "accepted"
            )
            if name == "unknown-call" and "line 2:1" not in reference.stderr:
                print(
                    f"{name}: verifier lost the parsed instruction source line",
                    file=sys.stderr,
                )
                print(reference.stderr, file=sys.stderr)
                return 1
            if (
                reference.returncode == 0
                or debug.returncode != 0
                or actual_reference_phase != phase
                or actual_lainir_phase != phase
                or compiled.returncode == 0
            ):
                print(
                    f"{name}: expected {phase}, C={actual_reference_phase}, "
                    f"LAIN-IR={debug_text!r}",
                    file=sys.stderr,
                )
                print(reference.stderr, file=sys.stderr)
                print(debug.stderr, file=sys.stderr)
                return 1

    print(f"LAIN-IR diagnostic differential: {len(CASES)} failure phases agreed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
