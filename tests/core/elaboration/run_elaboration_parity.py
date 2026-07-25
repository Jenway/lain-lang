#!/usr/bin/env python3
"""Require single-source and workspace elaboration to share semantics."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"
SELF_HOST = pathlib.Path(
    os.environ.get(
        "LAIN_TEST_COMPILER_ARTIFACT",
        ROOT / "build/core-self-hosting/stage2_compiler.l1",
    )
)
WORKSPACE_FIXTURES = ROOT / "tests/core/self_hosting/fixtures"


def tool(name: str) -> pathlib.Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out/bin" / f"{name}{suffix}"


def compile_and_run(
    sources: list[pathlib.Path], entry: str, workspace: bool = True
) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory(prefix="lain-elaboration-") as tmp:
        output = pathlib.Path(tmp) / "out.l1"
        mode = "--emit-workspace-l1" if workspace else "--emit-l1"
        result = subprocess.run(
            [
                str(tool("lainc")),
                "--artifact",
                str(SELF_HOST),
                mode,
                str(output) if workspace else str(sources[0]),
                *([str(path) for path in sources] if workspace else [str(output)]),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0 or not output.exists():
            return False, (result.stderr or result.stdout).strip()
        executed = subprocess.run(
            [str(tool("l1i")), str(output), entry],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        detail = executed.stdout.strip() or executed.stderr.strip()
        return executed.returncode == 0 and detail == "42", detail


def compile_error(source: pathlib.Path, workspace: bool) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory(prefix="lain-elaboration-error-") as tmp:
        output = pathlib.Path(tmp) / "out.l1"
        mode = "--emit-workspace-l1" if workspace else "--emit-l1"
        args = [
            str(tool("lainc")),
            "--artifact",
            str(SELF_HOST),
            mode,
        ]
        if workspace:
            args.extend((str(output), str(source)))
        else:
            args.extend((str(source), str(output)))
        result = subprocess.run(
            args, cwd=ROOT, capture_output=True, text=True
        )
        diagnostic = (result.stderr or result.stdout).strip()
        return result.returncode != 0 and not output.exists(), diagnostic


def main() -> int:
    cases = (
        (
            "cross-module member resolution",
            [
                WORKSPACE_FIXTURES / "math_module.lain",
                WORKSPACE_FIXTURES / "app_module.lain",
            ],
            "app__main",
        ),
        (
            "workspace consteval",
            [FIXTURES / "workspace_consteval.lain"],
            "app__main",
        ),
    )
    failed = 0
    for label, sources, entry in cases:
        ok, detail = compile_and_run(sources, entry)
        print(f"{'PASS' if ok else 'FAIL'} {label}: {detail}")
        failed += 0 if ok else 1

    single_ok, single_diagnostic = compile_error(
        FIXTURES / "single_bad_if.lain", False
    )
    workspace_ok, workspace_diagnostic = compile_error(
        FIXTURES / "workspace_bad_if.lain", True
    )
    parity = (
        single_ok
        and workspace_ok
        and "error 2401" in single_diagnostic
        and "error 2401" in workspace_diagnostic
    )
    print(
        f"{'PASS' if parity else 'FAIL'} diagnostic parity: "
        f"single={single_diagnostic!r}, workspace={workspace_diagnostic!r}"
    )
    failed += 0 if parity else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
