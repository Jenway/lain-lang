#!/usr/bin/env python3
"""M1 acceptance for structured Meta program/environment invocation."""

from __future__ import annotations

import os
import shutil
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
FIXTURE = ROOT / "tests" / "lainir_lain" / "fixtures" / "meta_invoke_environment.lain"
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
    directory = Path(tempfile.mkdtemp(prefix="lain-meta-invoke-"))
    try:
        temporary = directory
        gen1 = temporary / "gen1.l1"
        gen2 = temporary / "gen2.l1"
        product = temporary / "meta-invoke.l1"
        fixture = Path(os.environ.get("LAIN_META_FIXTURE", str(FIXTURE)))
        entry = os.environ.get("LAIN_META_ENTRY", "main")
        cached = ROOT / "build" / "debug-gen2-heap.l1"
        if os.environ.get("LAIN_META_USE_CACHED_GEN2") and cached.exists():
            gen2.write_bytes(cached.read_bytes())
        else:
            require(run(SEED, FROZEN, "compiler_compile", gen1, LAINC), "build gen1")
            require(
                run(SEED, "interpreter", gen1, "compiler_compile", gen2, LAINC),
                "build gen2",
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
                fixture,
            ),
            "compile Meta invocation fixture",
        )
        require(run(CHECK, product, entry), "verify Meta invocation fixture")
        executed = run(SEED, "run", product, entry)
        require(executed, "execute Meta invocation fixture")
        if executed.stdout.strip() != "0":
            raise RuntimeError(f"Meta invocation result: {executed.stdout.strip()!r}")
        print("PASS Meta program/environment scalar and syntax invocation")
        return 0
    finally:
        if not os.environ.get("LAIN_KEEP_META_INVOKE"):
            shutil.rmtree(directory, ignore_errors=True)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error)
        raise SystemExit(1)
