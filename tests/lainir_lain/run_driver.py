#!/usr/bin/env python3
"""Exercise the unified LAIN-IR Lain frontend driver."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
BOOTSTRAP = ROOT / "bootstrap" / "zig-out" / "bin"
L1BOOTSTRAP = BOOTSTRAP / "lainir-interpreter.exe"
L1CHECK = BOOTSTRAP / "lainir-print.exe"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
FRONTEND_RUNNER = ROOT / "scripts" / "run_lain_frontend.py"
TOOLS = ROOT / "src" / "lainir" / "tools"
LAIN = ROOT / "src" / "lainir" / "lain"
FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "driver.lain"
MATH = pathlib.Path(__file__).parent / "fixtures" / "module_math.lain"
APP = pathlib.Path(__file__).parent / "fixtures" / "module_app.lain"
MISSING = pathlib.Path(__file__).parent / "fixtures" / "module_missing.lain"
DIVZERO = pathlib.Path(__file__).parent / "fixtures" / "consteval_divzero.lain"
MODULE_VALUE = pathlib.Path(__file__).parent / "fixtures" / "module_value.lain"
CYCLE_A = pathlib.Path(__file__).parent / "fixtures" / "module_cycle_a.lain"
CYCLE_B = pathlib.Path(__file__).parent / "fixtures" / "module_cycle_b.lain"


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-driver-") as temporary:
        directory = pathlib.Path(temporary)
        bundle = directory / "driver_bundle.l1"
        output = directory / "driver.txt"
        bundled = run(
            [
                sys.executable,
                str(BUNDLER),
                "-o",
                str(bundle),
                str(TOOLS / "source.l1"),
                str(LAIN / "raw_ast.l1"),
                str(LAIN / "meta.l1"),
                str(LAIN / "module_meta.l1"),
                str(LAIN / "eval.l1"),
                str(LAIN / "workspace.l1"),
                str(LAIN / "workspace_cache.l1"),
                str(LAIN / "driver.l1"),
            ]
        )
        if bundled.returncode:
            print(bundled.stderr or bundled.stdout, file=sys.stderr)
            return 1
        checked = run([str(L1CHECK), str(bundle), "lain_compile_dump"])
        if checked.returncode:
            print(checked.stderr or checked.stdout, file=sys.stderr)
            return 1
        executed = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_compile_dump",
                str(output),
                str(FIXTURE),
            ]
        )
        if executed.returncode:
            print(executed.stderr or executed.stdout, file=sys.stderr)
            return 1
        text = output.read_text(encoding="utf-8")
        required = ("(lain 1", "(module ", "(record Point size=8", "(eval 42)")
        if any(item not in text for item in required):
            print(f"unified driver output is incomplete: {text!r}", file=sys.stderr)
            return 1
        public_output = directory / "public-driver.txt"
        public_run = run(
            [
                sys.executable,
                str(FRONTEND_RUNNER),
                "-o",
                str(public_output),
                str(FIXTURE),
            ]
        )
        if public_run.returncode:
            print(public_run.stderr or public_run.stdout, file=sys.stderr)
            return 1
        if public_output.read_text(encoding="utf-8") != text:
            print("public frontend entry differs from the bundled driver", file=sys.stderr)
            return 1
        module_output = directory / "module-driver.txt"
        module_run = run(
            [
                sys.executable,
                str(FRONTEND_RUNNER),
                "-o",
                str(module_output),
                str(MODULE_VALUE),
            ]
        )
        if module_run.returncode:
            print(module_run.stderr or module_run.stdout, file=sys.stderr)
            return 1
        if "(meta-module math members=2 (member add) (member zero))" not in module_output.read_text(encoding="utf-8"):
            print("public frontend omitted the module meta descriptor", file=sys.stderr)
            return 1
        ordered_output = directory / "ordered-driver.txt"
        reversed_output = directory / "reversed-driver.txt"
        for output, sources in (
            (ordered_output, (MATH, APP)),
            (reversed_output, (APP, MATH)),
        ):
            public_run = run(
                [
                    sys.executable,
                    str(FRONTEND_RUNNER),
                    "-o",
                    str(output),
                    *(str(source) for source in sources),
                ]
            )
            if public_run.returncode:
                print(public_run.stderr or public_run.stdout, file=sys.stderr)
                return 1
        if ordered_output.read_text(encoding="utf-8") != reversed_output.read_text(encoding="utf-8"):
            print("module order changed the unified driver artifact", file=sys.stderr)
            return 1
        for source, diagnostic in ((MISSING, "4101"), (DIVZERO, "3103")):
            bad_output = directory / f"bad-{source.stem}.txt"
            bad_run = run(
                [
                    sys.executable,
                    str(FRONTEND_RUNNER),
                    "-o",
                    str(bad_output),
                    str(source),
                ]
            )
            if bad_run.returncode == 0 or diagnostic not in (bad_run.stderr or bad_run.stdout):
                print(f"driver accepted {source.name}: {bad_run.stderr or bad_run.stdout}", file=sys.stderr)
                return 1
        cycle_output = directory / "cycle-driver.txt"
        cycle_run = run(
            [
                sys.executable,
                str(FRONTEND_RUNNER),
                "-o",
                str(cycle_output),
                str(CYCLE_B),
                str(CYCLE_A),
            ]
        )
        if cycle_run.returncode == 0 or "4103" not in (cycle_run.stderr or cycle_run.stdout):
            print(f"driver accepted module cycle: {cycle_run.stderr or cycle_run.stdout}", file=sys.stderr)
            return 1
    print("PASS unified Lain frontend driver")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
