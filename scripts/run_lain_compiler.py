#!/usr/bin/env python3
"""Compile the first Lain source subset to executable LAIN-IR text."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "scripts" / "build_lain_compiler.py"
EMPTY_SOURCE = ROOT / "scripts" / "fixtures" / "empty_source.lain"
L1BOOTSTRAP = seed_exe("lainir-seed")
BUNDLE = ROOT / "build" / "bootstrap" / "lainc.l1"
CORE_BUNDLE = ROOT / "build" / "bootstrap" / "compiler_core.l1"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"


def run(arguments: list[Path | str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output", required=True, type=Path)
    parser.add_argument(
        "--library",
        action="store_true",
        help="compile a source closure without requiring a native main entry",
    )
    parser.add_argument(
        "--stdlib-artifact",
        type=Path,
        help=(
            "use this standard-library artifact with compiler core instead "
            "of the default bootstrap stdlib"
        ),
    )
    parser.add_argument("source", type=Path, nargs="+")
    args = parser.parse_args()
    built = run([sys.executable, BUILD])
    if built.returncode:
        print(built.stderr or built.stdout, file=sys.stderr)
        return built.returncode
    selected_bundle = BUNDLE
    with tempfile.TemporaryDirectory(prefix="lain-stdlib-select-") as temp:
        if args.stdlib_artifact:
            stdlib = args.stdlib_artifact
            if not stdlib.is_absolute():
                stdlib = ROOT / stdlib
            if not stdlib.is_file():
                print(f"standard-library artifact not found: {stdlib}", file=sys.stderr)
                return 1
            selected_bundle = Path(temp) / "lain_compiler.l1"
            bundled = run(
                [
                    sys.executable,
                    BUNDLER,
                    "-o",
                    selected_bundle,
                    CORE_BUNDLE,
                    stdlib,
                ]
            )
            if bundled.returncode:
                print(bundled.stderr or bundled.stdout, file=sys.stderr)
                return bundled.returncode
        # The seed interpreter reserves the one-source form for an already
        # parsed LAIN-IR input.  Source compilation must therefore always
        # provide a second source unit, even when the caller requested one
        # Lain source file.  The empty unit has no declarations and does not
        # affect name resolution; it only selects the source-compiler path.
        sources = [ROOT / source for source in args.source]
        if len(sources) == 1:
            sources.append(EMPTY_SOURCE)
        executed = run(
            [
                L1BOOTSTRAP,
                selected_bundle,
                "compiler_compile_library" if args.library else "compiler_compile",
                args.output,
                *sources,
            ]
        )

        if executed.returncode:
            detail = executed.stderr or executed.stdout
            if args.output.is_file():
                output_text = args.output.read_text(encoding="utf-8")
                if output_text.lstrip().startswith("(error "):
                    detail = output_text.strip()
                args.output.unlink()
            print(detail, file=sys.stderr)
            return executed.returncode
        if not args.output.is_file():
            print(f"compiler did not write {args.output}", file=sys.stderr)
            return 1
        output_text = args.output.read_text(encoding="utf-8")
        if output_text.lstrip().startswith("(error "):
            print(output_text.strip(), file=sys.stderr)
            args.output.unlink()
            return 1
        verified = run([seed_exe("lainir-print"), args.output])
        if verified.returncode:
            print(verified.stderr or verified.stdout, file=sys.stderr)
            args.output.unlink()
            return verified.returncode
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
