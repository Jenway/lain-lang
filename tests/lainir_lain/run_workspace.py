#!/usr/bin/env python3
"""Exercise the LAIN-IR-written multi-source module boundary."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
BOOTSTRAP = ROOT / "seed" / "zig-out" / "bin"
L1BOOTSTRAP = BOOTSTRAP / "lainir-seed.exe"
L1CHECK = BOOTSTRAP / "lainir-print.exe"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
TOOLS = ROOT / "src" / "lainir" / "tools"
RAW_AST = ROOT / "src" / "lainir" / "lain" / "raw_ast.l1"
META = ROOT / "src" / "lainir" / "lain" / "meta.l1"
WORKSPACE = ROOT / "src" / "lainir" / "lain" / "workspace.l1"
FIXTURES = pathlib.Path(__file__).parent / "fixtures"


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def run_order(bundle: pathlib.Path, output: pathlib.Path, sources: tuple[pathlib.Path, ...]) -> str:
    executed = run(
        [
            str(L1BOOTSTRAP),
            str(bundle),
            "lain_workspace_dump",
            str(output),
            *(str(source) for source in sources),
        ]
    )
    if executed.returncode:
        raise RuntimeError(executed.stderr or executed.stdout)
    return output.read_text(encoding="utf-8")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-workspace-") as temporary:
        directory = pathlib.Path(temporary)
        bundle = directory / "workspace_bundle.l1"
        output = directory / "workspace.txt"
        bundled = run(
            [
                sys.executable,
                str(BUNDLER),
                "-o",
                str(bundle),
                str(TOOLS / "source.l1"),
                str(RAW_AST),
                str(META),
                str(WORKSPACE),
            ]
        )
        if bundled.returncode:
            print(bundled.stderr or bundled.stdout, file=sys.stderr)
            return 1
        checked = run([str(L1CHECK), str(bundle), "lain_workspace_dump"])
        if checked.returncode:
            print(checked.stderr or checked.stdout, file=sys.stderr)
            return 1
        math = FIXTURES / "module_math.lain"
        app = FIXTURES / "module_app.lain"
        ordered = run_order(bundle, output, (math, app))
        reversed_order = run_order(bundle, output, (app, math))
        expected_ordered = "(workspace 2 (module " + str(app) + " nodes=19 imports=1 resolved=1 unresolved=0 cycle=0 diagnostic=0) (module " + str(math) + " nodes=13 imports=0 resolved=0 unresolved=0 cycle=0 diagnostic=0))\n"
        expected_reversed = expected_ordered
        if ordered != expected_ordered or reversed_order != expected_reversed:
            print(f"unexpected workspace summary: {ordered!r} / {reversed_order!r}", file=sys.stderr)
            return 1
        missing = FIXTURES / "module_missing.lain"
        missing_summary = run_order(bundle, output, (missing, math))
        expected_missing = (
            "(module " + str(missing)
            + " nodes=6 imports=1 resolved=0 unresolved=1 cycle=0 diagnostic=4101)"
        )
        if expected_missing not in missing_summary:
            print(f"unresolved import was not diagnosed: {missing_summary!r}", file=sys.stderr)
            return 1
        cycle_a = FIXTURES / "module_cycle_a.lain"
        cycle_b = FIXTURES / "module_cycle_b.lain"
        cycle_summary = run_order(bundle, output, (cycle_a, cycle_b))
        if cycle_summary.count("cycle=1 diagnostic=4103") != 2:
            print(f"module cycle was not diagnosed: {cycle_summary!r}", file=sys.stderr)
            return 1
        self_module = FIXTURES / "module_self.lain"
        self_summary = run_order(bundle, output, (self_module,))
        if "imports=1 resolved=1 unresolved=0 cycle=1 diagnostic=4103" not in self_summary:
            print(f"self import was not diagnosed: {self_summary!r}", file=sys.stderr)
            return 1
    print("PASS Lain workspace RawAst/module boundary")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
