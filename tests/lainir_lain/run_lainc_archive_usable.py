#!/usr/bin/env python3
"""Require src/lainc's fixed-point compiler to produce a usable archive API.

This is intentionally stronger than the archive source-closure and stage-E
checks.  The generated program must retain the concrete compiler API, invoke
``api.compile`` at runtime, pass the L1 verifier, and execute successfully.
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
FROZEN = ROOT / "src" / "lainir" / "lainc.l1"
LAINC = ROOT / "src" / "lainc" / "lainc.lain"
FIXTURE = ROOT / "tests" / "lainir_lain" / "fixtures" / "compiler_api_compile_empty.lain"
FIXTURE_NONEMPTY = ROOT / "tests" / "lainir_lain" / "fixtures" / "compiler_api_compile_nonempty.lain"
# The archive compiler expects dependency-first input.  Alphabetical order
# places compiler_core before its factories and makes specialization both
# incorrect and dramatically slower.
ARCHIVE = tuple(
    ROOT / "src" / "compiler-archive" / name
    for name in (
        "tokenizer.lain",
        "syntax.lain",
        "generated_syntax.lain",
        "types.lain",
        "effects.lain",
        "compiler_context.lain",
        "modules.lain",
        "source_workspace.lain",
        "module_artifact.lain",
        "elaborator.lain",
        "l1_ir.lain",
        "l1_unit_builder.lain",
        "l1_verifier.lain",
        "l1_printer.lain",
        "l1_interpreter.lain",
        "meta.lain",
        "lower.lain",
        "workspace.lain",
        "frontend_pipeline.lain",
        "compiler.lain",
        "compiler_core.lain",
        "compiler_driver.lain",
        "compiler_api.lain",
        "lainc.lain",
    )
)
STD = (
    ROOT / "std" / "memory_model.lain",
    ROOT / "std" / "allocation.lain",
    ROOT / "std" / "bounds.lain",
    ROOT / "std" / "effect.lain",
    ROOT / "std" / "meta.lain",
    ROOT / "std" / "core" / "vec.lain",
    ROOT / "std" / "core" / "string.lain",
    ROOT / "std" / "core" / "arena_min.lain",
    ROOT / "std" / "core" / "slice.lain",
    ROOT / "std" / "core" / "source.lain",
    ROOT / "std" / "core" / "memory.lain",
)


def run(*arguments: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def require(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode:
        raise RuntimeError(f"{label}: {result.stderr or result.stdout}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lainc-archive-usable-") as directory:
        temporary = Path(directory)
        gen1 = temporary / "gen1.l1"
        gen2 = temporary / "gen2.l1"
        product = temporary / "compiler-api-empty.l1"
        product_nonempty = temporary / "compiler-api-nonempty.l1"

        require(run(SEED, FROZEN, "compiler_compile", gen1, LAINC), "build gen1")
        require(
            run(SEED, "interpreter", gen1, "compiler_compile", gen2, LAINC),
            "build gen2",
        )
        if gen2.stat().st_size < 1_000:
            raise RuntimeError(
                "build gen2 produced only bootstrap externs; seed interpreter "
                "did not provide source/artifact capabilities to compiler_compile"
            )
        require(
            run(
                SEED,
                "interpreter",
                gen2,
                "compiler_compile_library",
                product,
                *STD,
                *ARCHIVE,
                FIXTURE,
            ),
            "compile archive API client",
        )

        text = product.read_text(encoding="utf-8")
        if len(text) < 10_000:
            raise RuntimeError(
                f"archive API collapsed to a stub ({len(text)} bytes)"
            )
        if "#call" not in text or "_compile(" not in text:
            raise RuntimeError("archive API product does not retain api.compile")

        require(run(CHECK, product, "main"), "verify archive API client")
        executed = run(SEED, "run", product, "main")
        require(executed, "execute archive API client")
        if executed.stdout.strip() != "0":
            raise RuntimeError(
                f"archive api.compile(empty) returned {executed.stdout.strip()!r}"
            )

        # A non-empty source exercises the SourceWorkspace constructor and
        # the first real compiler pipeline call.  Keep this beside the empty
        # smoke so a verifier-only stub cannot regress into apparent success.
        require(
            run(
                SEED,
                "interpreter",
                gen2,
                "compiler_compile_library",
                product_nonempty,
                *STD,
                *ARCHIVE,
                FIXTURE_NONEMPTY,
            ),
            "compile archive API nonempty client",
        )
        require(run(CHECK, product_nonempty, "main"), "verify nonempty API client")
        executed_nonempty = run(SEED, "run", product_nonempty, "main")
        require(executed_nonempty, "execute nonempty archive API client")
        if executed_nonempty.stdout.strip() != "1":
            raise RuntimeError(
                "archive api.compile(nonempty) returned "
                f"{executed_nonempty.stdout.strip()!r}"
            )

    print("PASS src/lainc produces and executes empty and nonempty archive api.compile")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error)
        raise SystemExit(1)
