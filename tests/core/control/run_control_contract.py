#!/usr/bin/env python3
"""Exercise structured if-as-value lowering in single and workspace modes."""

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


def compile_single(
    source_name: str, expected_error: str | None = None
) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory(prefix="lain-control-") as tmp:
        output = pathlib.Path(tmp) / "out.l1"
        result = subprocess.run(
            [
                str(tool("lainc")), "--artifact", str(STAGE2),
                "--emit-l1", str(FIXTURES / source_name), str(output),
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
        value = executed.stdout.strip()
        return executed.returncode == 0 and value == "42", value or executed.stderr.strip()


def compile_workspace() -> tuple[bool, str]:
    with tempfile.TemporaryDirectory(prefix="lain-control-workspace-") as tmp:
        output = pathlib.Path(tmp) / "workspace.l1"
        result = subprocess.run(
            [
                str(tool("lainc")), "--artifact", str(STAGE2),
                "--emit-workspace-l1", str(output),
                str(FIXTURES / "workspace_if_value.lain"),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        diagnostic = (result.stderr or result.stdout).strip()
        if result.returncode != 0 or not output.exists():
            return False, diagnostic or "no workspace output"
        executed = subprocess.run(
            [str(tool("l1i")), str(output), "app__main"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        value = executed.stdout.strip()
        return executed.returncode == 0 and value == "42", value or executed.stderr.strip()


def main() -> int:
    cases = (
        ("local if value selects then", "if_value_true.lain", None),
        ("local if value selects else", "if_value_false.lain", None),
        ("nested if value", "if_value_nested.lain", None),
        ("if branch type mismatch", "if_value_type_mismatch.lain", "error 2402"),
        ("if condition must be bool", "if_value_bad_condition.lain", "error 2401"),
        ("local assignment", "assignment.lain", None),
        ("structured loop break continue", "loop_control.lain", None),
        ("loop body fallthrough repeats", "loop_fallthrough.lain", None),
        ("nested structured loop", "nested_loop.lain", None),
        ("assignment rejects parameter", "assignment_parameter.lain", "error 2410"),
        ("assignment type mismatch", "assignment_type_mismatch.lain", "error 2411"),
        ("break outside loop", "break_outside_loop.lain", "error 2412"),
        ("continue outside loop", "continue_outside_loop.lain", "error 2413"),
    )
    failed = 0
    for label, source, error in cases:
        ok, detail = compile_single(source, error)
        print(f"{'PASS' if ok else 'FAIL'} {label}: {detail}")
        failed += 0 if ok else 1
    ok, detail = compile_workspace()
    print(f"{'PASS' if ok else 'FAIL'} workspace if value: {detail}")
    failed += 0 if ok else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
