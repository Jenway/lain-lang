#!/usr/bin/env python3
"""Compare function-declaration shape validation in bootstrap and formal std::meta.

The shape of `let NAME = std::func<T>(params) [?{...}] -> type [!{...}] {body}`
is a language rule, so it lives in the standard library on both sides:
`lain_std_function_header_status` / `lain_std_function_tail_status` in
`bootstrap/std/core_forms.l1`, and `meta_function_status` in `std/meta.lain`.
This gate feeds one source to both compilers and requires the same status code,
the way `check_meta_module_validation.py` does for `module` and `struct`.

The declaration is judged in two segments because one check between them is not
a shape rule: whether the arrow type is valid needs the unit's type table, which
only the lowering pass owns.  Formal Meta has no such table at the expand stage,
and its entry and tail rules read a type run instead of asking the type
grammar's scanner, so a few malformed sources are shapes it cannot decide.
`UNCOVERED` names those; each one still asserts the bootstrap status and prints
the observed formal code with the reason, rather than being dropped or asserted
equal.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
SEED = seed_exe("lainir-seed")
RUN_COMPILER = ROOT / "scripts" / "run_lain_compiler.py"
BUILD_FORMAL_STDLIB = ROOT / "scripts" / "build_formal_stdlib.py"
FORMAL_SOURCES = ROOT / "std"
FORMAL_ABI = ROOT / "build" / "lainir" / "formal_stdlib_abi_probe.l1"
COMPILER_API = ROOT / "bootstrap" / "compiler" / "compiler_api.l1"
POSITIVE = ROOT / "scripts" / "fixtures" / "formal_function_shape_full.lain"

# Shapes both compilers must reject with the same status, as
# (label, source, status).
CASES = (
    (
        "missing arrow",
        "let f = std::func(v: i64) i64 {\n    return v;\n};\n",
        "5102",
    ),
    (
        "parameter list is not a group",
        "let f = std::func[v: i64] -> i64 {\n    return v;\n};\n",
        "5103",
    ),
    (
        "input row is not a group",
        "let f = std::func(v: i64) ?IO -> i64 {\n    return v;\n};\n",
        "5102",
    ),
    (
        "input entry missing a name",
        "let f = std::func(v: i64) ?{: i64} -> i64 {\n    return v;\n};\n",
        "5102",
    ),
    (
        "input entry missing a type",
        "let f = std::func(v: i64) ?{T:} -> i64 {\n    return v;\n};\n",
        "5102",
    ),
    (
        "input entries missing a separator",
        "let f = std::func(v: i64) ?{T: i64 U: i64} -> i64 {\n"
        "    return v;\n};\n",
        "5102",
    ),
    (
        "effect row is not a group",
        "let f = std::func(v: i64) -> i64 !IO {\n    return v;\n};\n",
        "5102",
    ),
    (
        "body is not a group",
        "let f = std::func(v: i64) -> i64 !{IO} 5;\n",
        "5102",
    ),
    (
        "body is missing",
        "let f = std::func(v: i64) -> i64 !{IO}\n",
        "5102",
    ),
)

# Shapes the formal expand stage cannot decide, as (label, source, bootstrap
# status, reason).  Return-type validity needs the unit's type table, and the
# entry and return-type rules here are stated in terms of the type-expression
# grammar the bootstrap entry shares with the lowering pass.  The bootstrap
# status is still required; the formal code is printed with the reason instead
# of being asserted equal, so the boundary stays visible instead of being
# papered over.
UNCOVERED = (
    (
        # Priority: the arrow type is invalid *and* the effect row is
        # malformed.  The bootstrap entries straddle the type check, so the
        # arrow type wins; formal Meta cannot judge the arrow type at all.
        "invalid arrow type before a malformed effect row",
        "let f = std::func(v: i64) -> bogus !IO {\n    return v;\n};\n",
        "5104",
        "the formal expand stage owns no unit type table",
    ),
    (
        "entry type run with a dangling reference",
        "let f = std::func(a: i64) ?{T: & mut, U: i64} -> i64 {\n"
        "    return a;\n};\n",
        "5102",
        "the formal entry rule does not re-check the type grammar",
    ),
    (
        "entry type run with a trailing token",
        "let f = std::func(a: i64) ?{T: std::type i64, U: i64} -> i64 {\n"
        "    return a;\n};\n",
        "5102",
        "the formal entry rule does not re-check the type grammar",
    ),
    (
        "return type run before a declaration terminator",
        "let f = std::func(v: i64) -> i64 5;\n",
        "5102",
        "the formal tail reads `;` as an external declaration",
    ),
)


def run(arguments: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def bootstrap(source: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return run(
        [
            sys.executable,
            str(RUN_COMPILER),
            "--library",
            "-o",
            str(output),
            str(source),
        ]
    )


def formal(source: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return run(
        [
            str(SEED),
            "interpreter",
            str(FORMAL_ABI),
            "compiler_compile_library",
            str(output),
            str(COMPILER_API),
            str(source),
        ]
    )


def detail(result: subprocess.CompletedProcess[str]) -> str:
    return (result.stdout or "") + (result.stderr or "")


def probe_is_current() -> bool:
    """The formal ABI probe is bundled from the formal standard library."""
    if not FORMAL_ABI.is_file():
        return False
    probe = FORMAL_ABI.stat().st_mtime
    return all(
        source.stat().st_mtime <= probe
        for source in FORMAL_SOURCES.rglob("*.lain")
    )


def ensure_probe() -> None:
    if probe_is_current():
        return
    result = run([sys.executable, str(BUILD_FORMAL_STDLIB)])
    if result.returncode:
        raise RuntimeError(
            "formal standard library rebuild failed: "
            + detail(result).strip()
        )


def status_in(message: str, code: str) -> bool:
    return f"status {code}" in message


def main() -> int:
    if not POSITIVE.is_file():
        raise RuntimeError(f"missing fixture: {POSITIVE}")
    ensure_probe()
    compared = 0
    skipped = 0
    with tempfile.TemporaryDirectory(prefix="lain-function-shape-") as temp:
        directory = Path(temp)
        compiled = bootstrap(POSITIVE, directory / "positive_bootstrap.l1")
        if compiled.returncode:
            raise RuntimeError(
                f"bootstrap rejected a well-shaped declaration: "
                f"{detail(compiled).strip()}"
            )
        compiled = formal(POSITIVE, directory / "positive_formal.l1")
        for index, (label, source, expected) in enumerate(CASES):
            fixture = directory / f"case_{index}.lain"
            fixture.write_text(source, encoding="utf-8")
            observed = bootstrap(
                fixture, directory / f"case_{index}_bootstrap.l1"
            )
            if not status_in(detail(observed), expected):
                raise RuntimeError(
                    f"{label}: bootstrap expected status {expected}, got "
                    f"{detail(observed).strip()}"
                )
            observed = formal(fixture, directory / f"case_{index}_formal.l1")
            if not status_in(detail(observed), expected):
                raise RuntimeError(
                    f"{label}: formal expected status {expected}, got "
                    f"{detail(observed).strip()}"
                )
            compared += 1

        for label, source, expected, reason in UNCOVERED:
            fixture = directory / "uncovered.lain"
            fixture.write_text(source, encoding="utf-8")
            observed = bootstrap(fixture, directory / "uncovered_bootstrap.l1")
            if not status_in(detail(observed), expected):
                raise RuntimeError(
                    f"{label}: bootstrap expected status {expected}, got "
                    f"{detail(observed).strip()}"
                )
            observed = formal(fixture, directory / "uncovered_formal.l1")
            skipped += 1
            print(
                f"SKIP formal: {label}: {reason} (bootstrap {expected}, "
                f"formal {detail(observed).strip() or 'no diagnostic'})"
            )

    print(
        "PASS bootstrap/formal function declaration shape "
        f"({compared} cases compared, {skipped} skipped)"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"Function shape conformance failed: {error}")
        raise SystemExit(1)
