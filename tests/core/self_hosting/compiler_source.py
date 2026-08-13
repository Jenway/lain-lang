#!/usr/bin/env python3
"""Define and materialize the canonical Lain compiler source closure.

Every bootstrap generation consumes this exact byte sequence.  Keeping the
module order and source concatenation here prevents stage-0, stage-1, and
stage-2 from accidentally proving a fixed point for different programs.
"""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "build/core-self-hosting/compiler_source.lain"
# The package-qualified names inside Lain imports are logical module names;
# the repository's canonical physical source root is src/compiler.
MODULES = (
    "src/compiler/tokenizer.lain",
    "src/compiler/syntax.lain",
    "src/compiler/generated_syntax.lain",
    "src/compiler/types.lain",
    "src/compiler/effects.lain",
    "src/compiler/compiler_context.lain",
    "src/compiler/modules.lain",
    "src/compiler/source_workspace.lain",
    "src/compiler/module_artifact.lain",
    "src/compiler/elaborator.lain",
    "src/compiler/l1_ir.lain",
    "src/compiler/l1_unit_builder.lain",
    "src/compiler/l1_verifier.lain",
    "src/compiler/l1_printer.lain",
    "src/compiler/l1_interpreter.lain",
    "src/compiler/meta.lain",
    "src/compiler/lower.lain",
    "src/compiler/workspace.lain",
    "src/compiler/frontend_pipeline.lain",
    "src/compiler/compiler.lain",
    "src/compiler/compiler_core.lain",
    "src/compiler/compiler_driver.lain",
    "src/compiler/compiler_api.lain",
    "src/compiler/lainc.lain",
)


def build_compiler_source() -> Path:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    sections = []
    for relative in MODULES:
        text = (ROOT / relative).read_text(encoding="utf-8")
        sections.append(text.rstrip("\r\n") + "\n")
    OUT.write_text("\n".join(sections), encoding="utf-8", newline="\n")
    return OUT


if __name__ == "__main__":
    print(build_compiler_source().relative_to(ROOT))
