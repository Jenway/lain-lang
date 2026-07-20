#!/usr/bin/env python3
"""Exercise Lain-owned std::enum definition, TypeValue and layout contracts."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"
STAGE2 = ROOT / "build/core-self-hosting/stage2_compiler.l1"


def tool(name: str) -> pathlib.Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out/bin" / f"{name}{suffix}"


def compile_case(
    source_name: str, expected_error: str | None
) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory(prefix="lain-enum-") as tmp:
        output = pathlib.Path(tmp) / "out.l1"
        result = subprocess.run(
            [
                str(tool("lainc")),
                "--artifact",
                str(STAGE2),
                "--emit-l1",
                str(FIXTURES / source_name),
                str(output),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        diagnostic = (result.stderr or result.stdout).strip()
        if expected_error is not None:
            ok = (
                result.returncode != 0
                and not output.exists()
                and expected_error in diagnostic
            )
            return ok, diagnostic or f"missing {expected_error}"
        if result.returncode != 0 or not output.exists():
            return False, diagnostic or "no output"
        executed = subprocess.run(
            [str(tool("l1i")), str(output), "main"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        ok = executed.returncode == 0 and executed.stdout.strip() == "42"
        return ok, executed.stdout.strip() or diagnostic


def layout_probe() -> tuple[bool, str]:
    result = subprocess.run(
        [
            str(tool("lainc")),
            "--artifact",
            str(STAGE2),
            "--artifact-run",
            "enum_layout_contract",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    value = result.stdout.strip()
    return result.returncode == 0 and value == "42", value or result.stderr.strip()


def main() -> int:
    cases = (
        ("generic enum definition", "definition.lain", None),
        ("specialized enum TypeValue", "specialized_type.lain", None),
        ("constructor and exhaustive match", "constructor_match.lain", None),
        ("malformed type parameter", "malformed_parameter.lain", "error 3001"),
        ("duplicate variant", "duplicate_variant.lain", "error 3002"),
        ("invalid payload", "invalid_payload.lain", "error 3003"),
        ("match target must be enum", "match_non_enum.lain", "error 3010"),
        ("unknown match variant", "match_unknown_variant.lain", "error 3011"),
        ("duplicate match variant", "match_duplicate_variant.lain", "error 3011"),
        ("non-exhaustive match", "match_non_exhaustive.lain", "error 3012"),
        ("unreachable match arm", "match_unreachable_arm.lain", "error 3013"),
        ("match arm type mismatch", "match_type_mismatch.lain", "error 3014"),
    )
    failed = 0
    for label, source, error in cases:
        ok, detail = compile_case(source, error)
        print(f"{'PASS' if ok else 'FAIL'} {label}: {detail}")
        failed += 0 if ok else 1
    ok, detail = layout_probe()
    print(f"{'PASS' if ok else 'FAIL'} deterministic layout: {detail}")
    failed += 0 if ok else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
