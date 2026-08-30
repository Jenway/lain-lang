#!/usr/bin/env python3
"""Static contract for the archive's typed structured L1 builder."""

from __future__ import annotations

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
BUILDER = ROOT / "src" / "compiler-archive" / "l1_unit_builder.lain"
VERIFIER = ROOT / "src" / "compiler-archive" / "l1_verifier.lain"
CANONICALIZER = ROOT / "scripts" / "canonicalize_lainir.py"


def main() -> int:
    source = BUILDER.read_text(encoding="utf-8")
    verifier = VERIFIER.read_text(encoding="utf-8")
    required = (
        "let valid_type = std::func",
        "let same_type = std::func",
        "let valid_expression = std::func",
        "if kind < 3 || kind > 11",
        "target.return_type",
        "target.arguments.length()",
        "let append_return_typed = std::func",
        "value_type: return_type",
        "then_region >= unit.regions.length()",
        "condition_type.bit_width != 1",
        "body >= unit.regions.length()",
        "procedure >= unit.procedures.length()",
    )
    for spelling in required:
        if spelling not in source:
            raise RuntimeError(f"structured builder guard missing: {spelling}")
    verifier_required = (
        "let same_type = std::func",
        "return 7205",
        "return 7206",
        "return 7207",
        "return 7208",
        "return 7209",
        "region(value, procedure.body, procedure.return_type)",
    )
    for spelling in verifier_required:
        if spelling not in verifier:
            raise RuntimeError(f"structured verifier guard missing: {spelling}")
    if not CANONICALIZER.is_file():
        raise RuntimeError("canonicalizer is required for structured/text parity")
    print("structured L1 builder validation contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
