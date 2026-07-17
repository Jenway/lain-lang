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
    "packages/lain/compiler/mini_syntax.lain",
    "packages/lain/compiler/mini_middle.lain",
    "packages/lain/compiler/mini_type.lain",
    "packages/lain/compiler/compiler_context.lain",
    "packages/lain/compiler/mini_module.lain",
    "packages/lain/compiler/mini_elaborate.lain",
    "packages/lain/compiler/l1_unit_builder.lain",
    "packages/lain/compiler/l1_interpreter.lain",
    "packages/lain/compiler/mini_lower_unit.lain",
    "packages/lain/compiler/mini_diagnostic.lain",
    "packages/lain/compiler/mini_workspace.lain",
    "packages/lain/compiler/source_workspace.lain",
    "packages/lain/compiler/module_artifact.lain",
    "packages/lain/compiler/frontend_pipeline.lain",
    "packages/lain/compiler/compiler_state.lain",
    "packages/lain/compiler/mini_meta.lain",
    "packages/lain/compiler/compiler_api.lain",
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
