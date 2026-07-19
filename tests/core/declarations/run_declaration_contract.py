#!/usr/bin/env python3
"""Track the single-declaration-form contract through the self-hosted CLI."""

from __future__ import annotations

import dataclasses
import os
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[3]
FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"


@dataclasses.dataclass(frozen=True)
class Case:
    name: str
    source: str
    mode: str
    expected_error: str | None = None


CASES = (
    Case("inferred function/value", "inferred_function.lain", "single"),
    Case("struct binding", "struct_binding.lain", "single"),
    Case("import binding", "import_binding.lain", "single"),
    Case("module binding", "module_binding.lain", "workspace"),
    Case(
        "reject old fn declaration",
        "reject_old_fn.lain",
        "single",
        "error 12001",
    ),
    Case(
        "reject old struct declaration",
        "reject_old_struct.lain",
        "single",
        "error 12001",
    ),
    Case(
        "reject old import declaration",
        "reject_old_import.lain",
        "single",
        "error 12001",
    ),
    Case(
        "reject uninferrable binding",
        "reject_uninferrable.lain",
        "single",
        "error 12007",
    ),
)


def compiler() -> pathlib.Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out" / "bin" / f"lainc{suffix}"


def exercise(case: Case, lainc: pathlib.Path, output: pathlib.Path) -> tuple[bool, str]:
    source = FIXTURES / case.source
    if case.mode == "workspace":
        command = [
            str(lainc),
            "--emit-workspace-l1",
            str(output),
            str(source),
        ]
    else:
        command = [str(lainc), "--emit-l1", str(source), str(output)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    diagnostic = (result.stderr or result.stdout).strip()

    if case.expected_error is None:
        if result.returncode == 0 and output.exists():
            return True, "compiled through the self-hosted CLI"
        return False, diagnostic or f"compiler exited {result.returncode}"

    if (
        result.returncode != 0
        and not output.exists()
        and case.expected_error in diagnostic
    ):
        return True, f"reported {case.expected_error}"
    if result.returncode == 0:
        return False, "obsolete declaration was accepted"
    if output.exists():
        return False, "compiler left a partial output after rejection"
    return False, diagnostic or f"missing diagnostic: {case.expected_error}"


def main() -> int:
    lainc = compiler()
    if not lainc.exists():
        print(f"FAIL compiler missing: {lainc}")
        return 1

    ready = 0
    failed = 0
    with tempfile.TemporaryDirectory(prefix="lain-declarations-") as tmp:
        out_dir = pathlib.Path(tmp)
        for index, case in enumerate(CASES):
            output = out_dir / f"{index}.l1"
            ok, detail = exercise(case, lainc, output)
            if ok:
                ready += 1
                print(f"READY {case.name}: {detail}")
            else:
                failed += 1
                print(f"FAIL {case.name}: {detail}")

    print(f"Canonical declarations: passed={ready}, failed={failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
