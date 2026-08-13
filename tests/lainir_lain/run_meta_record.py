#!/usr/bin/env python3
"""Exercise a meta-defined record form over the LAIN-IR RawAst."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
L1BOOTSTRAP = ROOT / "bootstrap" / "zig-out" / "bin" / "l1bootstrap.exe"
L1CHECK = ROOT / "bootstrap" / "zig-out" / "bin" / "l1check.exe"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
TOOLS = ROOT / "src" / "lainir" / "tools"
RAW_AST = ROOT / "src" / "lainir" / "lain" / "raw_ast.l1"
META = ROOT / "src" / "lainir" / "lain" / "meta.l1"
FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "record.lain"
BAD_FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "bad_record.lain"
LAYOUT_FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "record_layout.lain"
DUPLICATE_FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "duplicate_record.lain"
UNKNOWN_FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "unknown_record.lain"


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-meta-record-") as temporary:
        directory = pathlib.Path(temporary)
        bundle = directory / "meta_record_bundle.l1"
        output = directory / "meta_record.txt"
        bundled = run(
            [
                sys.executable,
                str(BUNDLER),
                "-o",
                str(bundle),
                str(TOOLS / "source.l1"),
                str(RAW_AST),
                str(META),
            ]
        )
        if bundled.returncode:
            print(bundled.stderr or bundled.stdout, file=sys.stderr)
            return 1
        checked = run([str(L1CHECK), str(bundle), "lain_meta_record_dump"])
        if checked.returncode:
            print(checked.stderr or checked.stdout, file=sys.stderr)
            return 1
        executed = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_meta_record_dump",
                str(output),
                str(FIXTURE),
            ]
        )
        if executed.returncode:
            print(executed.stderr or executed.stdout, file=sys.stderr)
            return 1
        actual = output.read_text(encoding="utf-8")
        expected = "(record Point size=8 (field x i32 offset=0) (field y i32 offset=4))\n"
        if actual != expected:
            print(f"unexpected meta record: {actual!r}", file=sys.stderr)
            return 1
        bad_output = directory / "bad_meta_record.txt"
        bad_executed = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_meta_record_dump",
                str(bad_output),
                str(BAD_FIXTURE),
            ]
        )
        if bad_executed.returncode:
            print(bad_executed.stderr or bad_executed.stdout, file=sys.stderr)
            return 1
        if bad_output.read_text(encoding="utf-8") != "(error 3004)\n":
            print(
                f"malformed let was accepted: {bad_output.read_text()!r}",
                file=sys.stderr,
            )
            return 1
        layout_output = directory / "layout_meta_record.txt"
        layout_executed = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_meta_record_dump",
                str(layout_output),
                str(LAYOUT_FIXTURE),
            ]
        )
        if layout_executed.returncode:
            print(layout_executed.stderr or layout_executed.stdout, file=sys.stderr)
            return 1
        expected_layout = (
            "(record Header size=24 (field tag i8 offset=0) "
            "(field count i32 offset=4) (field flags i16 offset=8) "
            "(field stamp i64 offset=16))\n"
        )
        if layout_output.read_text(encoding="utf-8") != expected_layout:
            print(f"unexpected mixed-layout record: {layout_output.read_text()!r}", file=sys.stderr)
            return 1
        duplicate_output = directory / "duplicate_meta_record.txt"
        duplicate_executed = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_meta_record_dump",
                str(duplicate_output),
                str(DUPLICATE_FIXTURE),
            ]
        )
        if duplicate_executed.returncode:
            print(duplicate_executed.stderr or duplicate_executed.stdout, file=sys.stderr)
            return 1
        if duplicate_output.read_text(encoding="utf-8") != "(error 3008)\n":
            print(f"duplicate field was accepted: {duplicate_output.read_text()!r}", file=sys.stderr)
            return 1
        unknown_output = directory / "unknown_meta_record.txt"
        unknown_executed = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_meta_record_dump",
                str(unknown_output),
                str(UNKNOWN_FIXTURE),
            ]
        )
        if unknown_executed.returncode:
            print(unknown_executed.stderr or unknown_executed.stdout, file=sys.stderr)
            return 1
        if unknown_output.read_text(encoding="utf-8") != "(error 3003)\n":
            print(f"unknown field type was accepted: {unknown_output.read_text()!r}", file=sys.stderr)
            return 1
    print("PASS Lain meta record slice")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
