#!/usr/bin/env python3
"""Check that the Lain-written C backend emits structurally sound C.

The backend is line oriented: emit_line recognises a construct by its line
prefix.  A branch that matches a prefix and returns without consuming the rest
of the line silently drops everything after it.  That is exactly how
`} else { #return #call f() }` lost its body and its closing brace, leaving a
function one brace short so the next function nested inside it.  The backend
exited 0 throughout, and the only symptom was zig cc cascading
"function definition is not allowed here" over the generated file.

This gate therefore inspects the emitted text directly, which reports the
defect at its source rather than 20 compiler errors later:

* braces balance, and
* no top-level function definition appears inside another function's body.

Braces inside string literals, character literals and comments are stripped
first; counting them raw gives a wrong answer for generated code, which is full
of brace characters inside string literals.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "build" / "seed" / "bin" / (
    "lainir-seed.exe" if sys.platform == "win32" else "lainir-seed"
)
BACKEND = ROOT / "build" / "backend_c_entry.l1"
FIXTURES = ROOT / "scripts" / "fixtures"
# The inline-else fixture is the regression case this check was written for.
CASES = (
    "backend_inline_else.l1",
    "backend_constant_return.l1",
)

_STRING = re.compile(r'"(\\.|[^"\\])*"')
_CHAR = re.compile(r"'(\\.|[^'\\])*'")
_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT = re.compile(r"//[^\n]*")
# A definition that starts at column zero and closes its parameter list on this
# line or the next.  Declarations end in `;` and are not definitions.
_TOP_LEVEL_DEF = re.compile(
    r"^(?:void|int|unsigned|uint\d+_t|int\d+_t|float|double|char|size_t|uintptr_t)\b"
)


def strip_noncode(text: str) -> str:
    text = _BLOCK_COMMENT.sub(" ", text)
    text = _LINE_COMMENT.sub(" ", text)
    text = _STRING.sub('""', text)
    text = _CHAR.sub("''", text)
    return text


def analyse(text: str) -> tuple[int, int]:
    """Return (final brace depth, top-level definitions seen inside a body)."""
    depth = 0
    nested = 0
    pending = False
    for line in strip_noncode(text).splitlines():
        stripped = line.strip()
        if pending and stripped.startswith(")"):
            pending = False
            if depth != 0:
                nested += 1
        elif _TOP_LEVEL_DEF.match(line):
            if "(" in line and ")" not in line:
                pending = True
            elif depth != 0:
                nested += 1
        depth += line.count("{") - line.count("}")
    return depth, nested


def generate(fixture: Path, output: Path) -> None:
    result = subprocess.run(
        [str(SEED), "interpreter", str(BACKEND), "main", str(output), str(fixture)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(
            f"{fixture.name}: backend failed: "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )
    if not output.is_file():
        raise RuntimeError(f"{fixture.name}: backend wrote no output")


def main() -> int:
    if not SEED.is_file() or not BACKEND.is_file():
        print(
            "backend C shape: build the seed and the backend entry first",
            file=sys.stderr,
        )
        return 2
    missing = [f for f in CASES if not (FIXTURES / f).is_file()]
    if missing:
        print("backend C shape: missing fixtures: " + ", ".join(missing), file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="lain-backend-shape-", dir=ROOT / "build") as raw:
        work = Path(raw)
        for name in CASES:
            output = work / f"{Path(name).stem}.c"
            generate(FIXTURES / name, output)
            text = output.read_text(encoding="utf-8", errors="replace")
            depth, nested = analyse(text)
            if depth != 0:
                print(
                    f"{name}: emitted C is not brace balanced "
                    f"(final depth {depth:+d})",
                    file=sys.stderr,
                )
                return 1
            if nested:
                print(
                    f"{name}: {nested} top-level definition(s) emitted inside "
                    f"another function body",
                    file=sys.stderr,
                )
                return 1
            print(f"PASS {name}: emitted C is balanced and flat")
    print("PASS backend C shape: balanced braces, no nested definitions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
