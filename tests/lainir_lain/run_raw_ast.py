#!/usr/bin/env python3
"""Exercise the Lain RawAst parser through the stage-0 LAIN-IR runner."""

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
PARSER = ROOT / "src" / "lainir" / "lain" / "raw_ast.l1"
FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "record.lain"


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-raw-ast-") as temporary:
        directory = pathlib.Path(temporary)
        bundle = directory / "raw_ast_bundle.l1"
        output = directory / "raw_ast.txt"
        bundled = run(
            [
                sys.executable,
                str(BUNDLER),
                "-o",
                str(bundle),
                str(TOOLS / "source.l1"),
                str(PARSER),
            ]
        )
        if bundled.returncode:
            print(bundled.stderr or bundled.stdout, file=sys.stderr)
            return 1
        checked = run([str(L1CHECK), str(bundle), "lain_raw_ast_dump"])
        if checked.returncode:
            print(checked.stderr or checked.stdout, file=sys.stderr)
            return 1
        executed = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_raw_ast_dump",
                str(output),
                str(FIXTURE),
            ]
        )
        if executed.returncode:
            print(executed.stderr or executed.stdout, file=sys.stderr)
            return 1
        text = output.read_text(encoding="utf-8")
        if not text.startswith("(root (atom let) (atom Point)"):
            print(f"unexpected RawAst prefix: {text!r}", file=sys.stderr)
            return 1
        if "(group {" not in text or "(group (" not in text:
            print(f"RawAst lost delimiter groups: {text!r}", file=sys.stderr)
            return 1
    print("PASS Lain RawAst parser stage0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
