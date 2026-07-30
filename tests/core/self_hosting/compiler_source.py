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
MODULES = (
    "packages/lain/compiler/tokenizer.lain",
    "packages/lain/compiler/syntax.lain",
    "packages/lain/compiler/generated_syntax.lain",
    "packages/lain/compiler/types.lain",
    "packages/lain/compiler/effects.lain",
    "packages/lain/compiler/compiler_context.lain",
    "packages/lain/compiler/modules.lain",
    "packages/lain/compiler/source_workspace.lain",
    "packages/lain/compiler/module_artifact.lain",
    "packages/lain/compiler/elaborator.lain",
    "packages/lain/compiler/l1_ir.lain",
    "packages/lain/compiler/l1_unit_builder.lain",
    "packages/lain/compiler/l1_verifier.lain",
    "packages/lain/compiler/l1_printer.lain",
    "packages/lain/compiler/l1_interpreter.lain",
    "packages/lain/compiler/meta.lain",
    "packages/lain/compiler/lower.lain",
    "packages/lain/compiler/workspace.lain",
    "packages/lain/compiler/frontend_pipeline.lain",
    "packages/lain/compiler/compiler.lain",
    "packages/lain/compiler/compiler_core.lain",
    "packages/lain/compiler/compiler_driver.lain",
    "packages/lain/compiler/compiler_api.lain",
    "packages/lain/compiler/lainc.lain",
    # Concrete transitional compiler entry. The modules above remain
    # compile-time library values until a concrete MemoryModel instantiates
    # them; this source is the physical compiler used for the bootstrap fixed
    # point during that transition.
    "packages/lain/bootstrap/stage1_compiler.lain",
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
