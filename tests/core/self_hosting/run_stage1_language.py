#!/usr/bin/env python3
"""Execute the Stage1 specialization and effect-checking gates."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

from full_closure import ROOT


BOOTSTRAP = Path(
    os.environ.get(
        "LAIN_BOOTSTRAP_ROOT",
        ROOT / "target" / "bootstrap-racket-worktree",
    )
).resolve()
BIN = BOOTSTRAP / "zig-out" / "bin"
FROZEN = BOOTSTRAP / "bootstrap" / "frozen" / "lainc.l1"
FIXTURES = ROOT / "tests" / "core" / "self_hosting" / "fixtures"
BUILD = ROOT / "build" / "core-self-hosting"


def tool(name: str) -> Path:
    return BIN / (name + (".exe" if os.name == "nt" else ""))


def run(*arguments: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        check=False,
        capture_output=True,
        text=True,
    )


def compile_fixture(name: str) -> tuple[subprocess.CompletedProcess[str], Path]:
    output = BUILD / f"{name}.l1"
    result = run(
        tool("l1bootstrap"),
        FROZEN,
        "compiler_compile",
        output,
        FIXTURES / f"{name}.lain",
    )
    return result, output


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    BUILD.mkdir(parents=True, exist_ok=True)

    compiled, specialization = compile_fixture("type_specialization")
    require(compiled.returncode == 0, compiled.stderr or compiled.stdout)
    text = specialization.read_text(encoding="utf-8")
    require("identity__T_i32" in text, "missing i32 specialization")
    require("identity__T_bool" in text, "missing bool specialization")
    require("identity__T_usize" in text, "missing usize specialization")
    require("identity__T_addr" in text, "missing addr specialization")
    require("%T" not in text, "compile-time type parameter leaked into L1")
    checked = run(tool("l1check"), specialization)
    require(checked.returncode == 0, checked.stderr or checked.stdout)
    executed = run(tool("l1i"), specialization, "main")
    require(executed.returncode == 0, executed.stderr or executed.stdout)
    require(executed.stdout.strip().endswith("42"), "specialization returned wrong value")

    present, effect_output = compile_fixture("effect_present")
    require(present.returncode == 0, present.stderr or present.stdout)
    require(
        "! {" not in effect_output.read_text(encoding="utf-8"),
        "effect syntax leaked into L1",
    )
    checked = run(tool("l1check"), effect_output)
    require(checked.returncode == 0, checked.stderr or checked.stdout)

    missing, _ = compile_fixture("effect_missing")
    require(missing.returncode != 0, "missing effect declaration was accepted")
    require(
        "status 3103" in missing.stderr,
        f"expected status 3103, got: {missing.stderr.strip()}",
    )

    print("stage1 language gates: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error)
        raise SystemExit(1)
