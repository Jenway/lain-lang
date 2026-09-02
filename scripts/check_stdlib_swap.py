#!/usr/bin/env python3
"""Prove that swapping bootstrap stdlib changes behavior without rebuilding core."""

from __future__ import annotations

import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "build" / "lainir" / "lain_compiler_core.l1"
STDLIB = ROOT / "build" / "lainir" / "bootstrap_std.l1"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
SEED = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)


def fail(message: str) -> int:
    print(f"stdlib swap check: {message}", file=sys.stderr)
    return 1


def main() -> int:
    if not CORE.is_file() or not STDLIB.is_file() or not SEED.is_file():
        return fail("build artifacts or seed interpreter are missing")

    core_hash = hashlib.sha256(CORE.read_bytes()).digest()
    stdlib = STDLIB.read_text(encoding="utf-8")
    marker = "#proc lain_std_meta_status"
    start = stdlib.find(marker)
    if start < 0:
        return fail("bootstrap stdlib has no lain_std_meta_status")
    end = stdlib.find("#proc ", start + len(marker))
    if end < 0:
        end = len(stdlib)
    body = stdlib[start:end]
    if "  #return 0" not in body:
        return fail("unexpected lain_std_meta_status body")
    swapped = stdlib[:start] + body.replace(
        "  #return 0", "  #return 5999", 1
    ) + stdlib[end:]

    with tempfile.TemporaryDirectory(prefix="lain-stdlib-swap-") as directory:
        work = Path(directory)
        compiler = work / "compiler.l1"
        swapped_std = work / "bootstrap_std_swapped.l1"
        output = work / "output.l1"
        swapped_std.write_text(swapped, encoding="utf-8", newline="\n")
        bundled = subprocess.run(
            [
                os.fspath(sys.executable),
                os.fspath(BUNDLER),
                "-o",
                os.fspath(compiler),
                os.fspath(CORE),
                os.fspath(swapped_std),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if bundled.returncode != 0:
            return fail(bundled.stderr.strip() or "failed to bundle swapped stdlib")
        result = subprocess.run(
            [
                os.fspath(SEED),
                "interpreter",
                os.fspath(compiler),
                "compiler_compile_library",
                os.fspath(output),
                os.fspath(ROOT / "std" / "meta.lain"),
                os.fspath(ROOT / "std" / "type.lain"),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if result.returncode == 0:
            return fail("swapped stdlib was not observed")
        diagnostics = result.stdout + result.stderr
        if "5999" not in diagnostics:
            return fail(
                "swapped stdlib returned an unexpected diagnostic: "
                + diagnostics.strip()
            )

        # A pass result owned by another context must be rejected at the
        # compiler boundary before the next pass sees it.
        expand_marker = "#proc lain_std_expand"
        expand_start = stdlib.find(expand_marker)
        if expand_start < 0:
            return fail("bootstrap stdlib has no lain_std_expand")
        expand_end = stdlib.find("#proc ", expand_start + len(expand_marker))
        if expand_end < 0:
            expand_end = len(stdlib)
        expand_body = stdlib[expand_start:expand_end]
        owner_position = expand_body.rfind("%owner")
        if owner_position < 0:
            return fail("unexpected expand result owner layout")
        owner_swapped = (
            stdlib[:expand_start]
            + expand_body[:owner_position]
            + "#call bootstrap.allocate-pages(1)"
            + expand_body[owner_position + len("%owner"):]
            + stdlib[expand_end:]
        )
        owner_std = work / "bootstrap_std_wrong_owner.l1"
        owner_compiler = work / "compiler_wrong_owner.l1"
        owner_output = work / "wrong_owner_output.l1"
        owner_std.write_text(owner_swapped, encoding="utf-8", newline="\n")
        owner_bundle = subprocess.run(
            [
                os.fspath(sys.executable),
                os.fspath(BUNDLER),
                "-o",
                os.fspath(owner_compiler),
                os.fspath(CORE),
                os.fspath(owner_std),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if owner_bundle.returncode != 0:
            return fail(
                owner_bundle.stderr.strip() or
                "failed to bundle wrong-owner stdlib"
            )
        owner_result = subprocess.run(
            [
                os.fspath(SEED),
                "interpreter",
                os.fspath(owner_compiler),
                "compiler_compile_library",
                os.fspath(owner_output),
                os.fspath(ROOT / "std" / "meta.lain"),
                os.fspath(ROOT / "std" / "type.lain"),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if owner_result.returncode == 0:
            return fail("wrong-owner stdlib result was accepted")
        owner_diagnostics = owner_result.stdout + owner_result.stderr
        if "5202" not in owner_diagnostics:
            return fail(
                "wrong-owner stdlib returned an unexpected diagnostic: "
                + owner_diagnostics.strip()
            )

    if hashlib.sha256(CORE.read_bytes()).digest() != core_hash:
        return fail("compiler core changed during stdlib swap")
    print("PASS bootstrap stdlib swap changes behavior without core rebuild")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
