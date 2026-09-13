#!/usr/bin/env python3
"""Compare library function/type syntax hooks and pipeline diagnostics.

Formal Meta parses references, qualified names, physical bits spelling and
ordinary factory-call/member groups when locating type boundaries. Positive
syntax is asserted through the actual formal hook, independently of the
incomplete formal lowering pass. Negative syntax must also produce the same
stable diagnostic through both compiler pipelines. Semantic return-type
validity before a malformed effect row still needs the formal type environment;
UNCOVERED preserves that remaining diagnostic-priority gap.
"""

from __future__ import annotations

import argparse
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
CASES = (('missing arrow', 'let f = std::func(v: i64) i64 {\n    return v;\n};\n', '5102'),
 ('parameter list is not a group',
  'let f = std::func[v: i64] -> i64 {\n    return v;\n};\n',
  '5103'),
 ('input row is not a group',
  'let f = std::func(v: i64) ?IO -> i64 {\n    return v;\n};\n',
  '5102'),
 ('input entry missing a name',
  'let f = std::func(v: i64) ?{: i64} -> i64 {\n    return v;\n};\n',
  '5102'),
 ('input entry missing a type',
  'let f = std::func(v: i64) ?{T:} -> i64 {\n    return v;\n};\n',
  '5102'),
 ('input entries missing a separator',
  'let f = std::func(v: i64) ?{T: i64 U: i64} -> i64 {\n    return v;\n};\n',
  '5102'),
 ('effect row is not a group',
  'let f = std::func(v: i64) -> i64 !IO {\n    return v;\n};\n',
  '5102'),
 ('body is not a group', 'let f = std::func(v: i64) -> i64 !{IO} 5;\n', '5102'),
 ('body is missing', 'let f = std::func(v: i64) -> i64 !{IO}\n', '5102'),
 ('entry type run with a dangling reference',
  'let f = std::func(a: i64) ?{T: & mut, U: i64} -> i64 {\n    return a;\n};\n',
  '5102'),
 ('entry type run with a trailing token',
  'let f = std::func(a: i64) ?{T: std::type i64, U: i64} -> i64 {\n    return a;\n};\n',
  '5102'),
 ('return type run before a declaration terminator',
  'let f = std::func(v: i64) -> i64 5;\n',
  '5102'))

# Semantic return-type validity and its diagnostic priority remain uncovered.
UNCOVERED = (('invalid arrow type before a malformed effect row',
  'let f = std::func(v: i64) -> bogus !IO {\n    return v;\n};\n',
  '5104',
  'the formal expand stage owns no unit type table'),)

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
    global FORMAL_ABI
    parser = argparse.ArgumentParser()
    parser.add_argument("--formal-abi", type=Path, help="explicit isolated core + formal library bundle")
    args = parser.parse_args()
    if args.formal_abi is not None:
        FORMAL_ABI = args.formal_abi.resolve()
    if not POSITIVE.is_file():
        raise RuntimeError(f"missing fixture: {POSITIVE}")
    if args.formal_abi is None:
        ensure_probe()
    cases = CASES
    uncovered = UNCOVERED
    compared = 0
    skipped = 0
    with tempfile.TemporaryDirectory(prefix="lain-function-shape-") as temp:
        directory = Path(temp)
        hook_bundle = directory / "formal_shape_hook.l1"
        bundled = run([sys.executable, str(ROOT / "scripts/bundle_lainir.py"), "-o", str(hook_bundle),
                       str(FORMAL_ABI), str(ROOT / "scripts/fixtures/formal_function_shape_probe.l1")])
        if bundled.returncode:
            raise RuntimeError(detail(bundled))
        def hook_status(source: Path, output: Path) -> str:
            result = run([str(SEED), "interpreter", str(hook_bundle), "main", str(output), str(source),
                          str(ROOT / "scripts/fixtures/empty_source.lain")])
            if result.returncode:
                raise RuntimeError(detail(result))
            return output.read_text(encoding="utf-8").strip()
        if hook_status(POSITIVE, directory / "positive_shape.txt") != "0":
            raise RuntimeError("formal Meta rejected the positive function syntax")
        # Syntax is checked independently of formal lowering, which does not
        # yet support every body/type in the positive fixture.
        for index, source in enumerate((
            "let f = std::func() ?{items: & mut ns::Vec(i64), limit: std::type} -> i64 { return 0; };\n",
            "let f = std::func() ?{bits: #bits<32>, T, ord: Ord(T)} -> Factory(T).Item;\n",
        )):
            fixture = directory / f"positive_type_{index}.lain"
            fixture.write_text(source, encoding="utf-8")
            if hook_status(fixture, directory / f"positive_type_{index}.txt") != "0":
                raise RuntimeError(f"formal Meta rejected positive type syntax {index}")
        compiled = bootstrap(POSITIVE, directory / "positive_bootstrap.l1")
        if compiled.returncode:
            raise RuntimeError(
                f"bootstrap rejected a well-shaped declaration: "
                f"{detail(compiled).strip()}"
            )
        compiled = formal(POSITIVE, directory / "positive_formal.l1")
        for index, (label, source, expected) in enumerate(cases):
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
            if hook_status(fixture, directory / f"case_{index}_shape.txt") != expected:
                raise RuntimeError(f"{label}: formal Meta hook expected {expected}")
            observed = formal(fixture, directory / f"case_{index}_formal.l1")
            if not status_in(detail(observed), expected):
                raise RuntimeError(
                    f"{label}: formal expected status {expected}, got "
                    f"{detail(observed).strip()}"
                )
            compared += 1

        for label, source, expected, reason in uncovered:
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
