#!/usr/bin/env python3
"""Prove that swapping the bootstrap Meta form vocabulary changes behavior.

Encoding 1c moved constructor spelling recognition out of the compiler and
behind the bootstrap standard library, so the compiler must ask
`lain_std_is_module_declaration` instead of comparing `module` itself.  This
check swaps that predicate's answer to 0, rebundles the compiler core with the
swapped stdlib, and requires a different compilation result.  If the results
still match, the compiler is deciding module forms on its own and the check
fails.
"""

from __future__ import annotations

import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "build" / "bootstrap" / "compiler_core.l1"
STDLIB = ROOT / "build" / "bootstrap" / "stdlib.l1"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
SEED = seed_exe("lainir-seed")
FIXTURE = ROOT / "scripts" / "fixtures" / "formal_meta_module_factory.lain"
EMPTY_SOURCE = ROOT / "scripts" / "fixtures" / "empty_source.lain"
MARKER = "#proc lain_std_is_module_declaration"


def fail(message: str) -> int:
    print(f"meta form swap check: {message}", file=sys.stderr)
    return 1


def bundle(output: Path, stdlib: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(BUNDLER),
            "-o",
            str(output),
            str(CORE),
            str(stdlib),
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        stdin=subprocess.DEVNULL,
    )


def compile_fixture(compiler: Path, output: Path) -> subprocess.CompletedProcess[str]:
    # The seed interpreter reserves the one-source form for already parsed
    # LAIN-IR, so source compilation always passes a second unit.
    return subprocess.run(
        [
            str(SEED),
            "interpreter",
            str(compiler),
            "compiler_compile_library",
            str(output),
            str(FIXTURE),
            str(EMPTY_SOURCE),
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        stdin=subprocess.DEVNULL,
    )


def main() -> int:
    if not CORE.is_file() or not STDLIB.is_file() or not SEED.is_file():
        return fail("build artifacts or seed interpreter are missing")

    core_hash = hashlib.sha256(CORE.read_bytes()).digest()
    stdlib = STDLIB.read_text(encoding="utf-8")
    start = stdlib.find(MARKER)
    if start < 0:
        return fail(f"bootstrap stdlib has no {MARKER.lstrip('#proc ')}")
    end = stdlib.find("#proc ", start + len(MARKER))
    if end < 0:
        end = len(stdlib)
    body = stdlib[start:end]
    if "#return 1" not in body:
        return fail("unexpected lain_std_is_module_declaration body")
    swapped = (
        stdlib[:start]
        + body.replace("#return 1", "#return 0", 1)
        + stdlib[end:]
    )

    with tempfile.TemporaryDirectory(prefix="lain-meta-form-swap-") as directory:
        work = Path(directory)
        plain_compiler = work / "compiler_plain.l1"
        swapped_compiler = work / "compiler_swapped.l1"
        swapped_std = work / "bootstrap_std_swapped.l1"
        plain_output = work / "plain_output.l1"
        swapped_output = work / "swapped_output.l1"

        plain_bundle = bundle(plain_compiler, STDLIB)
        if plain_bundle.returncode:
            return fail(
                plain_bundle.stderr.strip()
                or "failed to bundle the unswapped bootstrap compiler"
            )
        swapped_std.write_text(swapped, encoding="utf-8", newline="\n")
        swapped_bundle = bundle(swapped_compiler, swapped_std)
        if swapped_bundle.returncode:
            return fail(
                swapped_bundle.stderr.strip()
                or "failed to bundle the swapped bootstrap compiler"
            )

        plain = compile_fixture(plain_compiler, plain_output)
        if plain.returncode:
            return fail(
                "the unswapped bootstrap compiler failed: "
                + (plain.stderr or plain.stdout).strip()
            )
        if not plain_output.is_file():
            return fail("the unswapped bootstrap compiler wrote no artifact")

        result = compile_fixture(swapped_compiler, swapped_output)
        if (
            result.returncode == 0
            and swapped_output.is_file()
            and swapped_output.read_bytes() == plain_output.read_bytes()
        ):
            return fail(
                "the swapped lain_std_is_module_declaration was not observed; "
                "the compiler still decides module forms itself"
            )

        difference = f"unswapped status 0, swapped status {result.returncode}"
        diagnostics = (result.stderr or result.stdout).strip()
        if diagnostics:
            difference += f" ({diagnostics.splitlines()[-1]})"

    if hashlib.sha256(CORE.read_bytes()).digest() != core_hash:
        return fail("compiler core changed during the bootstrap stdlib swap")
    print(f"PASS bootstrap meta form swap changes recognition ({difference})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
