#!/usr/bin/env python3
"""Static contracts for the intentionally non-buildable pure-Lain cutover."""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[3]


def fail(message: str) -> None:
    raise RuntimeError(message)


def text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> int:
    required = (
        "std/effect.lain",
        "std/allocation.lain",
        "std/bounds.lain",
        "std/memory_model.lain",
        "std/core/memory.lain",
        "std/core/arena.lain",
        "std/core/vec.lain",
        "std/core/string.lain",
        "std/platform/api.lain",
        "std/platform/memory.lain",
        "packages/lain/compiler/effects.lain",
        "packages/lain/compiler/tokenizer.lain",
        "packages/lain/compiler/syntax.lain",
        "packages/lain/compiler/generated_syntax.lain",
        "packages/lain/compiler/meta.lain",
        "packages/lain/compiler/modules.lain",
        "packages/lain/compiler/source_workspace.lain",
        "packages/lain/compiler/module_artifact.lain",
        "packages/lain/compiler/elaborator.lain",
        "packages/lain/compiler/l1_ir.lain",
        "packages/lain/compiler/l1_unit_builder.lain",
        "packages/lain/compiler/l1_verifier.lain",
        "packages/lain/compiler/l1_printer.lain",
        "packages/lain/compiler/l1_interpreter.lain",
        "packages/lain/compiler/lower.lain",
        "packages/lain/compiler/compiler_core.lain",
        "packages/lain/compiler/compiler_driver.lain",
    )
    for relative in required:
        if not (ROOT / relative).is_file():
            fail(f"missing pure-Lain facility: {relative}")

    core_files = list((ROOT / "std/core").glob("*.lain"))
    for path in core_files:
        source = path.read_text(encoding="utf-8")
        if "std::platform" in source:
            fail(f"core imports platform: {path.relative_to(ROOT)}")
        if re.search(r"\b(malloc|realloc|free|mmap|VirtualAlloc|syscall)\b", source):
            fail(f"core names an OS/C allocator: {path.relative_to(ROOT)}")

    compiler_files = list((ROOT / "packages/lain/compiler").glob("*.lain"))
    platform_importers = []
    for path in compiler_files:
        source = path.read_text(encoding="utf-8")
        if "std::platform" in source:
            platform_importers.append(path.name)
    if platform_importers != ["compiler_driver.lain"]:
        fail(f"unexpected compiler platform imports: {platform_importers}")

    legacy_patterns = (
        r"\bhost_[A-Za-z0-9_]*",
        r"\bast_[A-Za-z0-9_]*",
        r"\bgenerated_host_[A-Za-z0-9_]*",
        r"\bmeta_host_[A-Za-z0-9_]*",
        r"\bM11[A-Za-z0-9_]*",
        r"\bm11_[A-Za-z0-9_]*",
        r"\bM9[A-Za-z0-9_]*",
        r"\bm9_[A-Za-z0-9_]*",
        r"\bl1_unit_builder_[A-Za-z0-9_]*",
    )
    for path in compiler_files:
        source_text = path.read_text(encoding="utf-8")
        for pattern in legacy_patterns:
            if re.search(pattern, source_text):
                fail(
                    f"legacy compiler interface remains in "
                    f"{path.relative_to(ROOT)}: {pattern}"
                )

    compiler_context = text("packages/lain/compiler/compiler_context.lain")
    compiler_api = text("packages/lain/compiler/compiler_api.lain")
    forbidden_storage = (
        "host_storage_",
        "compiler_api_storage_",
        "@foreign(c",
        "let Allocator: type",
    )
    for spelling in forbidden_storage:
        if spelling in compiler_context or spelling in compiler_api:
            fail(f"old host storage remains: {spelling}")

    all_lain = "\n".join(
        path.read_text(encoding="utf-8")
        for path in (ROOT / "std").rglob("*.lain")
    )
    all_lain += "\n" + "\n".join(
        path.read_text(encoding="utf-8") for path in compiler_files
    )
    forbidden_ir = (
        "#heap_alloc",
        "#syscall",
        "#call_capability",
        "heap_alloc(",
    )
    for spelling in forbidden_ir:
        if spelling in all_lain:
            fail(f"forbidden LAIN-IR/platform primitive: {spelling}")

    effects = text("packages/lain/compiler/effects.lain")
    for diagnostic in ("3101", "3102", "3103", "3104"):
        if diagnostic not in effects and diagnostic not in text(
            "packages/lain/compiler/diagnostics.lain"
        ):
            fail(f"missing effect diagnostic {diagnostic}")
    for operation in (
        "contains_all",
        "validate_call",
        "apply_handler",
        "validate_program",
    ):
        if operation not in effects:
            fail(f"missing effect operation {operation}")

    arena = text("std/core/arena.lain")
    if "memory.Region" not in arena or "let reset" not in arena:
        fail("Arena is not backed by caller-supplied bytes")
    if "allocation.Alloc(Policy)" not in arena:
        fail("Arena does not implement a parameterized allocation effect")
    if "std::handler(Policy.Effect)" not in arena:
        fail("Arena does not provide an allocation handler")

    vec = text("std/core/vec.lain")
    for spelling in (
        "let vec: Module",
        "Allocation: AllocationPolicy",
        "Bounds: BoundsPolicy",
        "std::type_with_namespace",
        "Allocation.Effect",
        "Bounds.Effect",
    ):
        if spelling not in vec:
            fail(f"Vec lacks module/policy contract: {spelling}")
    if "Result(" in vec or "allocator: Allocator" in vec:
        fail("Vec still mixes explicit result/allocator with effects")

    if (ROOT / "std/core/allocator.lain").exists():
        fail("obsolete Allocator callback module still exists")

    types = text("packages/lain/compiler/types.lain")
    for spelling in (
        "let types: Module",
        "let ModuleValue: type",
        "let NamespaceId: type",
        "namespace: NamespaceId",
        "module_payload: ModuleId",
    ):
        if spelling not in types:
            fail(f"type/module model lacks: {spelling}")

    l1 = text("packages/lain/compiler/l1_ir.lain")
    builder = text("packages/lain/compiler/l1_unit_builder.lain")
    interpreter = text("packages/lain/compiler/l1_interpreter.lain")
    if "let l1_ir: Module" not in l1 or "let Unit: type" not in l1:
        fail("dynamic L1 unit model is missing")
    if "host_" in builder or "host_" in interpreter:
        fail("L1 builder/interpreter still delegates storage or execution")
    if "first_region" not in l1 or "second_region" not in l1:
        fail("L1 structured regions are missing")
    for obsolete in (
        "l1_type.lain",
        "l1_value.lain",
        "l1_instruction.lain",
        "l1_region.lain",
        "l1_procedure.lain",
        "l1_unit.lain",
    ):
        if (ROOT / "packages/lain/compiler" / obsolete).exists():
            fail(f"obsolete fixed-size L1 mock remains: {obsolete}")

    tokenizer = text("packages/lain/compiler/tokenizer.lain")
    syntax = text("packages/lain/compiler/syntax.lain")
    generated = text("packages/lain/compiler/generated_syntax.lain")
    for spelling in (
        "let Tokenizer = std::func",
        "let tokenize = std::func",
        "let TokenStream: type",
    ):
        if spelling not in tokenizer:
            fail(f"pure tokenizer lacks: {spelling}")
    for spelling in (
        "let Syntax = std::func",
        "let Store: type",
        "let parse = std::func",
        "let token_equal = std::func",
    ):
        if spelling not in syntax:
            fail(f"pure syntax store lacks: {spelling}")
    if "origin:" not in generated or "hygiene:" not in generated:
        fail("generated syntax does not preserve origin and hygiene")

    modules = text("packages/lain/compiler/modules.lain")
    artifact = text("packages/lain/compiler/module_artifact.lain")
    for spelling in (
        "types.ModuleValue",
        "let resolve_import = std::func",
        "let resolve_member = std::func",
    ):
        if spelling not in modules:
            fail(f"module environment lacks: {spelling}")
    if ".lci" not in artifact or "historical .lci" not in artifact:
        fail("module artifact must explicitly reject the legacy bridge")

    effects = text("packages/lain/compiler/effects.lain")
    for spelling in (
        "argument_start: usize",
        "let Store: type",
        "let validate_call = std::func",
        "let apply_handler = std::func",
        "input - handled + remaining",
    ):
        if spelling not in effects:
            fail(f"parameterized effect model lacks: {spelling}")

    lower = text("packages/lain/compiler/lower.lain")
    core = text("packages/lain/compiler/compiler_core.lain")
    if "let Lower = std::func" not in lower:
        fail("single lowering engine is missing")
    if "Builder.add_procedure" not in lower:
        fail("lowering does not write the dynamic L1 model")
    for forbidden in ("compiler_parse_sources(", "compiler_lower_l1("):
        if forbidden in core:
            fail(f"compiler core still has a placeholder stage: {forbidden}")

    for obsolete in (
        "compiler_state.lain",
        "surface_forms.lain",
        "enums.lain",
    ):
        if (ROOT / "packages/lain/compiler" / obsolete).exists():
            fail(f"obsolete parallel compiler domain remains: {obsolete}")

    for fixture in (
        "tests/core/pure_lain/fixtures/return_42.lain",
        "tests/core/pure_lain/fixtures/module_policy.lain",
        "tests/core/pure_lain/fixtures/call_42.lain",
    ):
        if not (ROOT / fixture).is_file():
            fail(f"missing R1-R4 acceptance fixture: {fixture}")

    elaborator = text("packages/lain/compiler/elaborator.lain")
    for spelling in (
        "Modules.resolve_import",
        "program.module_bindings.push",
        "begin_specialization",
        "Effects.validate_program",
    ):
        if spelling not in elaborator:
            fail(f"elaborator does not connect R2/R3: {spelling}")

    source_buffer = text("std/core/source.lain")
    tokenizer = text("packages/lain/compiler/tokenizer.lain")
    core = text("packages/lain/compiler/compiler_core.lain")
    if "contents: memory.ByteSpan" not in source_buffer:
        fail("SourceBuffer still owns compiler source bytes")
    if "source: memory.ByteSpan" not in tokenizer:
        fail("tokenizer does not borrow source bytes")
    if "Meta.expand(" not in core:
        fail("normal compiler path skips Meta expansion")
    if "namespace: Module" in types or "transform: Module" in text(
        "packages/lain/compiler/meta.lain"
    ):
        fail("compile-time Module remains in runtime compiler storage")

    lower = text("packages/lain/compiler/lower.lain")
    interpreter = text("packages/lain/compiler/l1_interpreter.lain")
    if "expression.kind == 3" not in lower:
        fail("surface calls are not lowered")
    if "expression.kind == 20" not in interpreter:
        fail("L1 interpreter does not execute call expressions")
    if "procedure: ProcedureId" not in l1:
        fail("L1 calls do not use stable ProcedureId targets")
    if "expression.name" in interpreter or "expression.name" in lower:
        fail("L1 calls still resolve procedures through owned names")

    platform_memory = text("std/platform/memory.lain")
    if "path: Memory.String.clone(" not in platform_memory:
        fail("memory SourceRead handler shallow-copies an owned source path")
    if "artifact.*" in platform_memory:
        fail("memory ArtifactWrite handler shallow-copies borrowed bytes")
    if "return value.text;" in syntax:
        fail("generated token text escapes through a shallow owned copy")

    print("pure Lain contracts: PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as error:
        print(f"pure Lain contracts: FAIL: {error}")
        sys.exit(1)
