#!/usr/bin/env python3
"""Generate and execute LAIN-IR from the meta record descriptor."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
BOOTSTRAP = ROOT / "bootstrap" / "zig-out" / "bin"
L1BOOTSTRAP = BOOTSTRAP / "lainir-seed.exe"
L1CHECK = BOOTSTRAP / "lainir-print.exe"
L1I = BOOTSTRAP / "lainir-seed.exe"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
TOOLS = ROOT / "src" / "lainir" / "tools"
RAW_AST = ROOT / "src" / "lainir" / "lain" / "raw_ast.l1"
META = ROOT / "src" / "lainir" / "lain" / "meta.l1"
LOWER = ROOT / "src" / "lainir" / "lain" / "lower_record.l1"
FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "record.lain"
THREE_FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "record_three.lain"
MIXED_FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "record_mixed.lain"
UNSIGNED_FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "record_unsigned.lain"


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-lower-record-") as temporary:
        directory = pathlib.Path(temporary)
        bundle = directory / "lower_record_bundle.l1"
        output = directory / "record.l1"
        bundled = run(
            [
                sys.executable,
                str(BUNDLER),
                "-o",
                str(bundle),
                str(TOOLS / "source.l1"),
                str(RAW_AST),
                str(META),
                str(LOWER),
            ]
        )
        if bundled.returncode:
            print(bundled.stderr or bundled.stdout, file=sys.stderr)
            return 1
        checked = run([str(L1CHECK), str(bundle), "lain_lower_record"])
        if checked.returncode:
            print(checked.stderr or checked.stdout, file=sys.stderr)
            return 1
        generated = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_lower_record",
                str(output),
                str(FIXTURE),
            ]
        )
        if generated.returncode:
            print(generated.stderr or generated.stdout, file=sys.stderr)
            return 1
        output_check = run([str(L1CHECK), str(output), "main"])
        if output_check.returncode:
            print(output_check.stderr or output_check.stdout, file=sys.stderr)
            return 1
        executed = run([str(L1I), "run", str(output), "main"])
        if executed.returncode or executed.stdout.strip() != "42":
            print(executed.stderr or executed.stdout, file=sys.stderr)
            return 1
        three_output = directory / "record_three.l1"
        three_generated = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_lower_record",
                str(three_output),
                str(THREE_FIXTURE),
            ]
        )
        if three_generated.returncode:
            print(three_generated.stderr or three_generated.stdout, file=sys.stderr)
            return 1
        three_checked = run([str(L1CHECK), str(three_output), "main"])
        if three_checked.returncode:
            print(three_checked.stderr or three_checked.stdout, file=sys.stderr)
            return 1
        three_executed = run([str(L1I), "run", str(three_output), "main"])
        if three_executed.returncode or three_executed.stdout.strip() != "42":
            print(three_executed.stderr or three_executed.stdout, file=sys.stderr)
            return 1
        mixed_output = directory / "record_mixed.l1"
        mixed_generated = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_lower_record",
                str(mixed_output),
                str(MIXED_FIXTURE),
            ]
        )
        if mixed_generated.returncode:
            print(mixed_generated.stderr or mixed_generated.stdout, file=sys.stderr)
            return 1
        mixed_checked = run([str(L1CHECK), str(mixed_output), "main"])
        if mixed_checked.returncode:
            print(mixed_checked.stderr or mixed_checked.stdout, file=sys.stderr)
            return 1
        mixed_executed = run([str(L1I), "run", str(mixed_output), "main"])
        if mixed_executed.returncode or mixed_executed.stdout.strip() != "42":
            print(mixed_executed.stderr or mixed_executed.stdout, file=sys.stderr)
            return 1
        unsigned_output = directory / "record_unsigned.l1"
        unsigned_generated = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_lower_record",
                str(unsigned_output),
                str(UNSIGNED_FIXTURE),
            ]
        )
        if unsigned_generated.returncode:
            print(unsigned_generated.stderr or unsigned_generated.stdout, file=sys.stderr)
            return 1
        unsigned_text = unsigned_output.read_text(encoding="utf-8")
        if "#zext[#bits<64>](#load[#bits<8>]" not in unsigned_text:
            print("unsigned field was not zero-extended", file=sys.stderr)
            return 1
        if "#sext[#bits<64>](#load[#bits<8>]" not in unsigned_text:
            print("signed field was not sign-extended", file=sys.stderr)
            return 1
        unsigned_checked = run([str(L1CHECK), str(unsigned_output), "main"])
        if unsigned_checked.returncode:
            print(unsigned_checked.stderr or unsigned_checked.stdout, file=sys.stderr)
            return 1
        unsigned_executed = run([str(L1I), "run", str(unsigned_output), "main"])
        if unsigned_executed.returncode or unsigned_executed.stdout.strip() != "42":
            print(unsigned_executed.stderr or unsigned_executed.stdout, file=sys.stderr)
            return 1
    print("PASS Lain record lowering and execution")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
