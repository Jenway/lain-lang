#!/usr/bin/env python3
"""Exercise the compile-time evaluator slice over generic syntax."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
L1BOOTSTRAP = ROOT / "bootstrap" / "zig-out" / "bin" / "lainir-interpreter.exe"
L1CHECK = ROOT / "bootstrap" / "zig-out" / "bin" / "lainir-print.exe"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
TOOLS = ROOT / "src" / "lainir" / "tools"
RAW_AST = ROOT / "src" / "lainir" / "lain" / "raw_ast.l1"
META = ROOT / "src" / "lainir" / "lain" / "meta.l1"
EVAL = ROOT / "src" / "lainir" / "lain" / "eval.l1"
FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "consteval.lain"
BAD_FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "consteval_bad.lain"
OPS_FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "consteval_ops.lain"
DIVZERO_FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "consteval_divzero.lain"


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-meta-eval-") as temporary:
        directory = pathlib.Path(temporary)
        bundle = directory / "meta_eval_bundle.l1"
        output = directory / "meta_eval.txt"
        bundled = run(
            [
                sys.executable,
                str(BUNDLER),
                "-o",
                str(bundle),
                str(TOOLS / "source.l1"),
                str(RAW_AST),
                str(META),
                str(EVAL),
            ]
        )
        if bundled.returncode:
            print(bundled.stderr or bundled.stdout, file=sys.stderr)
            return 1
        checked = run([str(L1CHECK), str(bundle), "lain_meta_eval_dump"])
        if checked.returncode:
            print(checked.stderr or checked.stdout, file=sys.stderr)
            return 1
        executed = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_meta_eval_dump",
                str(output),
                str(FIXTURE),
            ]
        )
        if executed.returncode:
            print(executed.stderr or executed.stdout, file=sys.stderr)
            return 1
        if output.read_text(encoding="utf-8") != "(eval 42)\n":
            print(f"unexpected eval result: {output.read_text()!r}", file=sys.stderr)
            return 1
        bad_output = directory / "meta_eval_bad.txt"
        bad_executed = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_meta_eval_dump",
                str(bad_output),
                str(BAD_FIXTURE),
            ]
        )
        if bad_executed.returncode:
            print(bad_executed.stderr or bad_executed.stdout, file=sys.stderr)
            return 1
        if bad_output.read_text(encoding="utf-8") != "(error 3101)\n":
            print(f"unsupported eval was accepted: {bad_output.read_text()!r}", file=sys.stderr)
            return 1
        ops_output = directory / "ops_eval.txt"
        ops_executed = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_meta_eval_dump",
                str(ops_output),
                str(OPS_FIXTURE),
            ]
        )
        if ops_executed.returncode:
            print(ops_executed.stderr or ops_executed.stdout, file=sys.stderr)
            return 1
        if ops_output.read_text(encoding="utf-8") != "(eval 41)\n":
            print(f"unexpected arithmetic consteval: {ops_output.read_text()!r}", file=sys.stderr)
            return 1
        divzero_output = directory / "divzero_eval.txt"
        divzero_executed = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_meta_eval_dump",
                str(divzero_output),
                str(DIVZERO_FIXTURE),
            ]
        )
        if divzero_executed.returncode:
            print(divzero_executed.stderr or divzero_executed.stdout, file=sys.stderr)
            return 1
        if divzero_output.read_text(encoding="utf-8") != "(error 3103)\n":
            print(f"division by zero was not diagnosed: {divzero_output.read_text()!r}", file=sys.stderr)
            return 1
    print("PASS Lain compile-time eval slice")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
