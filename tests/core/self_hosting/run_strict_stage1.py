#!/usr/bin/env python3
"""Prove that the transitional compiler no longer skips unknown bindings."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
BOOTSTRAP = Path(
    os.environ.get(
        "LAIN_BOOTSTRAP_ROOT",
        ROOT / "target" / "bootstrap-racket-worktree",
    )
).resolve()
BIN = BOOTSTRAP / "zig-out" / "bin"
FROZEN = BOOTSTRAP / "bootstrap" / "frozen" / "lainc.l1"
STAGE1_SOURCE = ROOT / "packages" / "lain" / "bootstrap" / "stage1_compiler.lain"
FIXTURES = ROOT / "tests" / "core" / "self_hosting" / "fixtures"
BUILD = ROOT / "build" / "core-self-hosting"


def tool(name: str) -> Path:
    return BIN / (name + (".exe" if os.name == "nt" else ""))


def run(*arguments: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        check=False,
        capture_output=True,
        text=True,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    BUILD.mkdir(parents=True, exist_ok=True)
    strict = BUILD / "strict_stage1.l1"
    accepted = BUILD / "strict_return_42.l1"
    multi = BUILD / "strict_multi_source.l1"
    imported = BUILD / "strict_import.l1"
    rejected = BUILD / "strict_unsupported.l1"

    generated = run(
        tool("l1bootstrap"),
        FROZEN,
        "compiler_compile",
        strict,
        STAGE1_SOURCE,
    )
    require(generated.returncode == 0, generated.stderr or generated.stdout)
    checked = run(tool("l1check"), strict, "compiler_compile")
    require(checked.returncode == 0, checked.stderr or checked.stdout)

    good = run(
        tool("l1bootstrap"),
        strict,
        "compiler_compile",
        accepted,
        ROOT / "tests" / "core" / "pure_lain" / "fixtures" / "return_42.lain",
    )
    require(good.returncode == 0, good.stderr or good.stdout)
    checked_good = run(tool("l1check"), accepted, "main")
    require(checked_good.returncode == 0, checked_good.stderr or checked_good.stdout)

    multi_result = run(
        tool("l1bootstrap"),
        strict,
        "compiler_compile",
        multi,
        FIXTURES / "multi_math.lain",
        FIXTURES / "multi_main.lain",
    )
    require(
        multi_result.returncode == 0,
        multi_result.stderr or multi_result.stdout,
    )
    checked_multi = run(tool("l1check"), multi, "main")
    require(
        checked_multi.returncode == 0,
        checked_multi.stderr or checked_multi.stdout,
    )
    executed_multi = run(tool("l1i"), multi, "main")
    require(
        executed_multi.returncode == 0
        and executed_multi.stdout.strip() == "42",
        executed_multi.stderr or executed_multi.stdout,
    )

    imported_result = run(
        tool("l1bootstrap"),
        strict,
        "compiler_compile",
        imported,
        FIXTURES / "imported_module.lain",
        FIXTURES / "import_resolved.lain",
    )
    require(
        imported_result.returncode == 0,
        imported_result.stderr or imported_result.stdout,
    )
    checked_import = run(tool("l1check"), imported, "main")
    require(
        checked_import.returncode == 0,
        checked_import.stderr or checked_import.stdout,
    )

    missing_import = run(
        tool("l1bootstrap"),
        strict,
        "compiler_compile",
        BUILD / "strict_missing_import.l1",
        FIXTURES / "import_missing.lain",
    )
    require(missing_import.returncode != 0, "missing import was accepted")
    require(
        "status 5007" in missing_import.stderr,
        f"expected status 5007, got: {missing_import.stderr.strip()}",
    )

    bad = run(
        tool("l1bootstrap"),
        strict,
        "compiler_compile",
        rejected,
        FIXTURES / "unsupported_top_level.lain",
    )
    require(bad.returncode != 0, "unknown top-level binding was silently accepted")
    require(
        "status 5001" in bad.stderr,
        f"expected status 5001, got: {bad.stderr.strip()}",
    )

    print("strict transitional compiler gate: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
