#!/usr/bin/env python3
"""Exercise the meta-owned module descriptor."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
BOOTSTRAP = ROOT / "bootstrap" / "zig-out" / "bin"
L1BOOTSTRAP = BOOTSTRAP / "l1bootstrap.exe"
L1CHECK = BOOTSTRAP / "l1check.exe"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
TOOLS = ROOT / "src" / "lainir" / "tools"
LAIN = ROOT / "src" / "lainir" / "lain"
FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "module_value.lain"
DUPLICATE = pathlib.Path(__file__).parent / "fixtures" / "duplicate_module.lain"


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-meta-module-") as temporary:
        directory = pathlib.Path(temporary)
        bundle = directory / "module_bundle.l1"
        output = directory / "module.txt"
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
            ]
        )
        if bundled.returncode:
            print(bundled.stderr or bundled.stdout, file=sys.stderr)
            return 1
        checked = run([str(L1CHECK), str(bundle), "lain_meta_module_dump"])
        if checked.returncode:
            print(checked.stderr or checked.stdout, file=sys.stderr)
            return 1
        executed = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_meta_module_dump",
                str(output),
                str(FIXTURE),
            ]
        )
        if executed.returncode:
            print(executed.stderr or executed.stdout, file=sys.stderr)
            return 1
        expected = "(meta-module math members=2 (member add) (member zero))\n"
        if output.read_text(encoding="utf-8") != expected:
            print(f"unexpected module descriptor: {output.read_text()!r}", file=sys.stderr)
            return 1
        duplicate_output = directory / "duplicate-module.txt"
        duplicate = run(
            [
                str(L1BOOTSTRAP),
                str(bundle),
                "lain_meta_module_dump",
                str(duplicate_output),
                str(DUPLICATE),
            ]
        )
        if duplicate.returncode:
            print(duplicate.stderr or duplicate.stdout, file=sys.stderr)
            return 1
        if duplicate_output.read_text(encoding="utf-8") != "(error 3013)\n":
            print(f"duplicate module member was accepted: {duplicate_output.read_text()!r}", file=sys.stderr)
            return 1
    print("PASS Lain meta module slice")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
