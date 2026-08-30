"""Canonicalize textual LAIN-IR artifacts for fixed-point comparisons.

The canonical form deliberately changes formatting only: extern declarations
are sorted, procedure blocks are sorted by their declared label, line endings
are LF, trailing whitespace is removed, and the document ends with one LF.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def _brace_delta(line: str) -> int:
    """Count structural braces while ignoring braces inside string literals."""
    delta = 0
    quoted = False
    escaped = False
    for char in line:
        if escaped:
            escaped = False
            continue
        if quoted and char == "\\":
            escaped = True
            continue
        if char == '"':
            quoted = not quoted
            continue
        if not quoted:
            if char == "{":
                delta += 1
            elif char == "}":
                delta -= 1
    return delta


def _blocks(text: str) -> tuple[list[str], list[str], list[str]]:
    lines = [line.strip() for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n")]
    lines = [line for line in lines if line.strip()]
    externs: list[str] = []
    procedures: list[str] = []
    other: list[str] = []
    index = 0
    while index < len(lines):
        line = lines[index]
        if line.startswith("#extern "):
            externs.append(line)
            index += 1
            continue
        if line.startswith("#proc "):
            block = [line]
            depth = _brace_delta(line)
            index += 1
            while index < len(lines) and depth > 0:
                block.append(lines[index])
                depth += _brace_delta(lines[index])
                index += 1
            if depth != 0:
                raise ValueError("unterminated #proc block")
            procedures.append("\n".join(block))
            continue
        other.append(line)
        index += 1
    return externs, procedures, other


def canonicalize(text: str) -> str:
    externs, procedures, other = _blocks(text)
    procedures.sort(key=lambda block: block.split("#proc ", 1)[1].split("(", 1)[0])
    chunks = sorted(externs) + other + procedures
    return "\n\n".join(chunk.strip() for chunk in chunks if chunk.strip()) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    args = parser.parse_args()
    result = canonicalize(args.input.read_text(encoding="utf-8"))
    if args.output:
        args.output.write_text(result, encoding="utf-8", newline="\n")
    else:
        print(result, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
