#!/usr/bin/env python3
"""Run Bootstrap Core acceptance slices.

Pending cases are reported, not counted as pass. Use --strict-pending when
0.1.0 is ready to require every listed slice.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
BOOTSTRAP = ROOT / "tests" / "bootstrap-core"
CASES = BOOTSTRAP / "cases.txt"


@dataclasses.dataclass(frozen=True)
class Case:
    suite: str
    name: str
    status: str
    mode: str
    path: pathlib.Path | None
    contract: str


PREPARE_INTERFACES: dict[tuple[str, str], tuple[str, ...]] = {}


def compiler_path() -> pathlib.Path:
    names = ("lainc.exe", "lainc")
    for directory in (ROOT / "zig-out" / "bin", ROOT / "src" / "compiler-archive"):
        for name in names:
            candidate = directory / name
            if candidate.exists():
                return candidate
    return ROOT / "zig-out" / "bin" / names[0]


def parse_cases() -> list[Case]:
    cases: list[Case] = []
    for line_no, raw in enumerate(CASES.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("|", 5)
        if len(parts) != 6:
            raise SystemExit(f"{CASES}:{line_no}: expected 6 pipe-separated fields")
        suite, name, status, mode, rel_path, contract = parts
        path = BOOTSTRAP / rel_path if rel_path else None
        cases.append(Case(suite, name, status, mode, path, contract))
    return cases


def extract_checks(path: pathlib.Path) -> tuple[list[str], list[str]]:
    expected: list[str] = []
    forbidden: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if "// CHECK:" in line:
            expected.append(line.split("// CHECK:", 1)[1].strip())
        if "// CHECK-NOT:" in line:
            forbidden.append(line.split("// CHECK-NOT:", 1)[1].strip())
    return expected, forbidden


def run_emit_l1(case: Case, compiler: pathlib.Path) -> tuple[bool, str]:
    assert case.path is not None
    out_dir = ROOT / "build" / "bootstrap-core"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_l1 = out_dir / f"{case.suite}-{case.name}.l1"
    prepared = prepare_case_interfaces(case, compiler)
    if prepared[0] is False:
        return prepared

    try:
        result = subprocess.run(
            [str(compiler), "--bootstrap-emit-l1", str(case.path), str(out_l1)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            return False, result.stderr.strip() or result.stdout.strip()
    finally:
        cleanup_case_interfaces(case)

    text = out_l1.read_text(encoding="utf-8")
    expected, forbidden = extract_checks(case.path)
    for pattern in expected:
        if pattern not in text:
            return False, f"missing CHECK pattern: {pattern}"
    for pattern in forbidden:
        if pattern in text:
            return False, f"unexpected CHECK-NOT pattern: {pattern}"
    return True, str(out_l1.relative_to(ROOT))


def prepare_case_interfaces(case: Case, compiler: pathlib.Path) -> tuple[bool, str]:
    for rel in PREPARE_INTERFACES.get((case.suite, case.name), ()):
        source = ROOT / rel
        out_lci = ROOT / f"{rel}.lci"
        result = subprocess.run(
            [str(compiler), "--emit-interface", str(source), str(out_lci)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            cleanup_case_interfaces(case)
            return False, result.stderr.strip() or result.stdout.strip()
    return True, ""


def cleanup_case_interfaces(case: Case) -> None:
    for rel in PREPARE_INTERFACES.get((case.suite, case.name), ()):
        path = ROOT / f"{rel}.lci"
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def run_check(case: Case, compiler: pathlib.Path) -> tuple[bool, str]:
    assert case.path is not None
    out_dir = ROOT / "build" / "bootstrap-core"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_c = out_dir / f"{case.suite}-{case.name}.c"
    result = subprocess.run(
        [str(compiler), str(case.path), str(out_c)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return False, result.stderr.strip() or result.stdout.strip()
    return True, str(out_c.relative_to(ROOT))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--strict-pending",
        action="store_true",
        help="treat pending Bootstrap Core slices as failures",
    )
    args = parser.parse_args()

    compiler = compiler_path()
    if not compiler.exists():
        print(f"FAIL compiler missing: {compiler}", flush=True)
        return 1

    passed = 0
    failed = 0
    pending = 0

    print("Bootstrap Core", flush=True)
    print("==============", flush=True)

    for case in parse_cases():
        label = f"{case.suite}/{case.name}"
        if case.status == "pending":
            pending += 1
            print(f"PENDING {label}: {case.contract}", flush=True)
            continue
        if case.status != "active":
            failed += 1
            print(f"FAIL {label}: unknown status {case.status}", flush=True)
            continue
        if case.path is None or not case.path.exists():
            failed += 1
            print(f"FAIL {label}: case file missing", flush=True)
            continue

        if case.mode == "emit-l1":
            ok, detail = run_emit_l1(case, compiler)
        elif case.mode == "check":
            ok, detail = run_check(case, compiler)
        else:
            ok, detail = False, f"unknown mode {case.mode}"

        if ok:
            passed += 1
            print(f"PASS {label}: {detail}", flush=True)
        else:
            failed += 1
            print(f"FAIL {label}: {detail}", flush=True)

    if args.strict_pending and pending:
        failed += pending

    print("\nBootstrap Core summary", flush=True)
    print("======================", flush=True)
    print(f"passed: {passed}", flush=True)
    print(f"failed: {failed}", flush=True)
    print(f"pending: {pending}", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
