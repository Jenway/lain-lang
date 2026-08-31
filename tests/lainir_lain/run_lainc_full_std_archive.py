#!/usr/bin/env python3
"""Compile the required formal std tree and archive compiler with gen2.

This is the cold/full counterpart to run_lainc_archive_usable.py.  The file
order is dependency-first so failures point at the first unsupported module,
instead of being hidden by a verifier-only or API-only fixture.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "seed" / "zig-out" / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""
SEED = BIN / f"lainir-seed{SUFFIX}"
CHECK = BIN / f"lainir-print{SUFFIX}"
COMPILER = ROOT / "build" / "debug-gen2-heap.l1"
FIXTURE = ROOT / "tests" / "lainir_lain" / "fixtures" / "compiler_api_compile_nonempty.lain"


def paths(*names: str) -> tuple[Path, ...]:
    return tuple(ROOT / name for name in names)


# Imports in formal std are intentionally listed before their users.  The
# bootstrap-only examples are included too: they are source files, not part
# of the compiler's hidden implementation.
STD = paths(
    # Keep the order used by the proven archive-usable gate first.  The
    # compiler resolves imported factories while specializing a source unit,
    # so this order is part of the current bootstrap contract.
    "std/memory_model.lain",
    "std/allocation.lain",
    "std/bounds.lain",
    "std/effect.lain",
    "std/meta.lain",
    "std/core/vec.lain",
    "std/core/string.lain",
    "std/core/arena_min.lain",
    "std/core/slice.lain",
    "std/core/source.lain",
    "std/core/memory.lain",
    # The remaining formal modules are currently independent of the archive
    # pipeline; keeping them after the proven prefix isolates new failures.
    "std/type.lain",
    "std/backend.lain",
    "std/control.lain",
    "std/diagnostic.lain",
    "std/generic.lain",
    "std/core/arena.lain",
    "std/core/result.lain",
    "std/mem.lain",
    "std/intrusive.lain",
    "std/platform/api.lain",
    "std/platform/memory.lain",
    "std/bootstrap/ast_tree.lain",
    "std/bootstrap/ir_builder.lain",
    "std/bootstrap/type_shape.lain",
)

# Language-level ownership and borrow checking are deliberately outside the
# current self-hosting milestone.  Include this only for an explicit optional
# experiment; it must not block the required archive path.
OPTIONAL_STD = paths("std/ownership.lain")

ARCHIVE = paths(
    "src/compiler-archive/tokenizer.lain",
    "src/compiler-archive/syntax.lain",
    "src/compiler-archive/generated_syntax.lain",
    "src/compiler-archive/types.lain",
    "src/compiler-archive/effects.lain",
    "src/compiler-archive/compiler_context.lain",
    "src/compiler-archive/modules.lain",
    "src/compiler-archive/source_workspace.lain",
    "src/compiler-archive/module_artifact.lain",
    "src/compiler-archive/elaborator.lain",
    "src/compiler-archive/l1_ir.lain",
    "src/compiler-archive/l1_unit_builder.lain",
    "src/compiler-archive/l1_verifier.lain",
    "src/compiler-archive/l1_printer.lain",
    "src/compiler-archive/l1_interpreter.lain",
    "src/compiler-archive/meta.lain",
    "src/compiler-archive/lower.lain",
    "src/compiler-archive/workspace.lain",
    "src/compiler-archive/frontend_pipeline.lain",
    "src/compiler-archive/compiler.lain",
    "src/compiler-archive/compiler_core.lain",
    "src/compiler-archive/compiler_driver.lain",
    "src/compiler-archive/compiler_api.lain",
    "src/compiler-archive/lainc.lain",
)


def run(*arguments: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=int(os.environ.get("LAIN_FULL_STD_TIMEOUT", "240")),
    )


def run_with_env(
    environment: dict[str, str], *arguments: Path | str
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=int(os.environ.get("LAIN_FULL_STD_TIMEOUT", "240")),
        env=environment,
    )


def main() -> int:
    compiler = Path(os.environ.get("LAIN_FULL_COMPILER", str(COMPILER)))
    if not compiler.is_absolute():
        compiler = ROOT / compiler
    if not compiler.exists():
        raise RuntimeError(f"missing compiler artifact: {compiler}")
    selected_std = STD
    if os.environ.get("LAIN_FULL_INCLUDE_OPTIONAL") == "1":
        selected_std += OPTIONAL_STD
    count = int(os.environ.get("LAIN_FULL_STD_COUNT", str(len(selected_std))))
    if count < 1 or count > len(selected_std):
        raise RuntimeError(f"LAIN_FULL_STD_COUNT must be 1..{len(selected_std)}")
    fixture = Path(os.environ.get("LAIN_FULL_FIXTURE", str(FIXTURE)))
    if not fixture.is_absolute():
        fixture = ROOT / fixture
    keep_output = os.environ.get("LAIN_FULL_STD_OUT")
    refeed = os.environ.get("LAIN_FULL_REFEED_ARTIFACT")
    refeed_expected = os.environ.get("LAIN_FULL_REFEED_EXPECTED", "0")
    refeed_path = Path(refeed) if refeed else None
    if refeed_path is not None and not refeed_path.is_absolute():
        refeed_path = ROOT / refeed_path
    if keep_output:
        product = Path(keep_output)
        product.parent.mkdir(parents=True, exist_ok=True)
        context = None
    else:
        context = tempfile.TemporaryDirectory(prefix="lainc-full-std-archive-")
        product = Path(context.name) / "archive-api.l1"
    try:
        generated = run(
            SEED,
            "interpreter",
            compiler,
            "compiler_compile_library",
            product,
            *selected_std[:count],
            *(() if os.environ.get("LAIN_FULL_NO_ARCHIVE") == "1" else ARCHIVE),
            fixture,
        )
        if generated.returncode:
            raise RuntimeError(generated.stderr or generated.stdout)
        checked = run(CHECK, product, "main")
        if checked.returncode:
            raise RuntimeError(checked.stderr or checked.stdout)
        execution_env = os.environ.copy()
        if refeed_path is not None:
            execution_env["LAINIR_RUN_ARTIFACT"] = str(refeed_path)
        executed = run_with_env(execution_env, SEED, "run", product, "main")
        if executed.returncode or executed.stdout.strip() != "0":
            raise RuntimeError(
                f"full std/archive API returned {executed.stdout.strip()!r}: "
                f"{executed.stderr}"
            )
        if refeed_path is not None:
            if not refeed_path.exists() or refeed_path.stat().st_size == 0:
                raise RuntimeError("compiler API did not emit a refeed artifact")
            refeed_checked = run(CHECK, refeed_path, "main")
            if refeed_checked.returncode:
                raise RuntimeError(refeed_checked.stderr or refeed_checked.stdout)
            refeed_executed = run(SEED, "run", refeed_path, "main")
            if refeed_executed.returncode or refeed_executed.stdout.strip() != refeed_expected:
                raise RuntimeError(
                    f"refeed artifact returned {refeed_executed.stdout.strip()!r}, "
                    f"expected {refeed_expected!r}: "
                    f"{refeed_executed.stderr}"
                )
    finally:
        if context is not None:
            context.cleanup()
    print("PASS required formal std tree and archive compile through gen2")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(error)
        raise SystemExit(1)
