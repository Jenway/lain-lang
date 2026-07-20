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

def specialization_deduplicates() -> tuple[bool, str]:
    source = FIXTURES / "generic_dedup.lain"
    with tempfile.TemporaryDirectory(prefix="lain-specialization-") as tmp:
        output = pathlib.Path(tmp) / "out.l1"
        result = subprocess.run(
            [str(compiler()), "--emit-l1", str(source), str(output)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0 or not output.exists():
            return False, (result.stderr or result.stdout).strip()
        text = output.read_text(encoding="utf-8")
        count = text.count("#proc identity__T_i32(")
        return count == 1, f"{count} concrete definition(s)"

def specialization_depth_is_bounded() -> tuple[bool, str]:
    definitions: list[str] = []
    for index in range(130):
        target = f"depth_{index + 1}" if index < 129 else ""
        result = (
            f"{target}(T, value)"
            if target
            else "value"
        )
        definitions.append(
            f"let depth_{index} = std::func(T: type, value: T) -> T {{\n"
            f"    return {result};\n"
            f"}};\n"
        )
    definitions.append(
        "let main = std::func() -> i32 {\n"
        "    return depth_0(i32, 1);\n"
        "};\n"
    )
    with tempfile.TemporaryDirectory(prefix="lain-specialization-depth-") as tmp:
        source = pathlib.Path(tmp) / "depth.lain"
        output = pathlib.Path(tmp) / "out.l1"
        source.write_text("\n".join(definitions), encoding="utf-8")
        result = subprocess.run(
            [str(compiler()), "--emit-l1", str(source), str(output)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        diagnostic = (result.stderr or result.stdout).strip()
        ok = (
            result.returncode != 0
            and not output.exists()
            and "error 2811" in diagnostic
        )
        return ok, diagnostic or "missing error 2811"


def main() -> int:
    cases = (
        ("literal Comptime(i32)", "literal_i32.lain", None),
        ("consteval user call", "consteval_call.lain", None),
        ("top-level consteval binding", "top_level_consteval.lain", None),
        ("type specialization", "generic_identity.lain", None),
        ("value specialization", "generic_repeat.lain", None),
        ("recursive specialization", "generic_recursive.lain", None),
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
    ok, detail = specialization_deduplicates()
    print(f"{'PASS' if ok else 'FAIL'} specialization dedup: {detail}")
    failed += 0 if ok else 1
    ok, detail = specialization_depth_is_bounded()
    print(f"{'PASS' if ok else 'FAIL'} specialization depth: {detail}")
    failed += 0 if ok else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
