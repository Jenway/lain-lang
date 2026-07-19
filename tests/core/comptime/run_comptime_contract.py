#!/usr/bin/env python3
"""Exercise the public Comptime(T) surface contract."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"


def compiler() -> pathlib.Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out" / "bin" / f"lainc{suffix}"


def run_case(
    source_name: str, expected_error: str | None
) -> tuple[bool, str]:
    source = FIXTURES / source_name
    with tempfile.TemporaryDirectory(prefix="lain-comptime-") as tmp:
        output = pathlib.Path(tmp) / "out.l1"
        result = subprocess.run(
            [str(compiler()), "--emit-l1", str(source), str(output)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        diagnostic = (result.stderr or result.stdout).strip()
        if expected_error is None:
            return (
                result.returncode == 0 and output.exists(),
                diagnostic or "compiled",
            )
        return (
            result.returncode != 0
            and not output.exists()
            and expected_error in diagnostic,
            diagnostic or f"missing {expected_error}",
        )


def main() -> int:
    cases = (
        ("literal Comptime(i32)", "literal_i32.lain", None),
        ("consteval user call", "consteval_call.lain", None),
        ("top-level consteval binding", "top_level_consteval.lain", None),
        ("reject Comptime(addr)", "malformed_addr.lain", "error 2801"),
        ("require compile-time value", "value_required.lain", "error 2802"),
        (
            "reject runtime dependency",
            "runtime_dependency.lain",
            "error 2803",
        ),
        (
            "reject foreign capability",
            "foreign_capability.lain",
            "error 2805",
        ),
    )
    failed = 0
    for label, source, error in cases:
        ok, detail = run_case(source, error)
        print(f"{'PASS' if ok else 'FAIL'} {label}: {detail}")
        failed += 0 if ok else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
