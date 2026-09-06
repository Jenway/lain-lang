#!/usr/bin/env python3
"""Check the source-level lainc -> LAINIR capability boundary."""

from __future__ import annotations

import re
from pathlib import Path

from lainc_sources import compiler_source_names, lainir_api_sources


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "src" / "lainir" / "api_contract.lain"
ROADMAP = ROOT / "docs" / "roadmaps" / "lainc-lainir-api-migration.md"
LEGACY_MODULES = (
    "src/lainc/l1_ir.lain",
    "src/lainc/l1_unit_builder.lain",
    "src/lainc/l1_verifier.lain",
    "src/lainc/l1_printer.lain",
    "src/lainc/l1_interpreter.lain",
)
REQUIRED_SHAPES = ("BuilderShape", "ArtifactShape", "EvalShape")
REQUIRED_RULES = (
    "schema_version",
    "SourceLocation",
    "Diagnostic",
    "new_unit",
    "finish",
    "verify",
    "diagnostic_code",
    "diagnostic_location",
    "write_canonical_text",
    "evaluate",
    "Capabilities",
    "empty_capabilities",
    "external_call_capabilities",
    "signed_divide",
    "unsigned_divide",
    "zero_extend",
    "sign_extend",
)
REQUIRED_PROVIDER_BUILDER = (
    "new_unit", "discard", "bits_type", "float_type", "addr_type",
    "unit_type", "never_type", "add_data", "declare_extern",
    "begin_procedure", "end_procedure", "begin_region", "add_parameter",
    "integer_literal", "procedure_address", "data_address",
    "add", "subtract", "multiply", "signed_divide", "unsigned_divide",
    "equal", "not_equal", "signed_less", "unsigned_less",
    "signed_less_equal", "unsigned_less_equal", "signed_greater",
    "unsigned_greater", "signed_greater_equal", "unsigned_greater_equal",
    "float_add", "float_subtract", "float_multiply", "float_divide",
    "float_equal", "float_less", "zero_extend", "sign_extend", "truncate",
    "bitcast", "call", "append_call_argument", "call_indirect", "alloca",
    "lea", "load", "store", "append_if", "append_loop", "append_break",
    "append_continue", "append_return", "finish",
)
REQUIRED_PROVIDER_ARTIFACT = (
    "verify", "diagnostic_code", "diagnostic_location",
    "write_canonical_text", "hash", "equal",
)
REQUIRED_PROVIDER_EVAL = (
    "default_limits", "make_i32", "make_bits", "value_type", "value_i32",
    "value_bits",
    "make_result", "result_status", "result_value", "evaluate",
    "empty_capabilities", "external_call_capabilities",
)


def main() -> int:
    failures: list[str] = []
    if not CONTRACT.is_file():
        failures.append("missing src/lainir/api_contract.lain")
        contract_text = ""
    else:
        contract_text = CONTRACT.read_text(encoding="utf-8")

    for name in REQUIRED_SHAPES + REQUIRED_RULES:
        if not re.search(rf"\b{re.escape(name)}\b", contract_text):
            failures.append(f"contract is missing {name}")

    if not ROADMAP.is_file():
        failures.append("missing API migration roadmap")

    provider_path = ROOT / "src" / "lainir" / "api" / "default_provider.lain"
    provider_text = provider_path.read_text(encoding="utf-8") if provider_path.is_file() else ""
    for name in (
        REQUIRED_PROVIDER_BUILDER
        + REQUIRED_PROVIDER_ARTIFACT
        + REQUIRED_PROVIDER_EVAL
    ):
        if not re.search(rf"\blet\s+{re.escape(name)}\b", provider_text):
            failures.append(f"default provider is missing {name}")

    sources = compiler_source_names(ROOT)
    provider_sources = tuple(
        path.relative_to(ROOT).as_posix() for path in lainir_api_sources(ROOT)
    )
    if "src/lainir/api_contract.lain" not in provider_sources:
        failures.append("LAINIR provider closure does not include the API contract")
    leaked = [name for name in sources if name.startswith("src/lainir/")]
    failures.extend(
        f"compiler-owned manifest contains provider source: {name}" for name in leaked
    )
    missing_sources = [name for name in sources if not (ROOT / name).is_file()]
    failures.extend(f"manifest source is missing: {name}" for name in missing_sources)

    # During migration these modules still exist, but their set is frozen.  A
    # final-mode invocation turns the same checker into the removal gate.
    import sys

    if "--final" in sys.argv:
        for name in LEGACY_MODULES:
            if (ROOT / name).exists() or name in sources:
                failures.append(f"legacy compiler-owned LAINIR module remains: {name}")
        for path in (ROOT / "src" / "lainc").glob("*.lain"):
            text = path.read_text(encoding="utf-8")
            if re.search(r'compiler::l1_(?:ir|unit_builder|verifier|printer|interpreter)', text):
                failures.append(f"concrete LAINIR import remains: {path.relative_to(ROOT)}")
            if re.search(r"\bIr\.(?:L1|Access|Interpreter)\b", text):
                failures.append(f"legacy provider surface remains: {path.relative_to(ROOT)}")
            if "DirectUnitFacts" in text:
                failures.append(f"direct unit inspection remains: {path.relative_to(ROOT)}")

        lower_text = (ROOT / "src" / "lainc" / "lower.lain").read_text(
            encoding="utf-8"
        )
        if re.search(r"\bBuilder\.(?:binary|convert)\b", lower_text):
            failures.append("lowering uses a raw operation kind")
        for field in ("types", "expressions", "regions", "procedures"):
            if re.search(rf"\bunit\s*\.\s*{field}\b", lower_text):
                failures.append(f"lowering reads provider storage field {field}")

        meta_text = (ROOT / "src" / "lainc" / "meta.lain").read_text(
            encoding="utf-8"
        )
        for pattern in (
            r"(?:return|push\s*\()\s*Eval\.Value\s*\{",
            r"(?:return|=)\s*Eval\.Result\s*\{",
            r"\bresult\s*\.\s*(?:status|value)\b",
            r"\bvalue\s*\.\s*(?:type_id|i32_value|bits_value)\b",
            r"\bEval\.external_call_capabilities\s*\(",
        ):
            if re.search(pattern, meta_text):
                failures.append(
                    "Meta reads Eval layout or grants itself external capability"
                )
                break

        bootstrap_text = (
            ROOT / "src" / "lainc" / "bootstrap_lainc.lain"
        ).read_text(encoding="utf-8")
        for spelling in (
            "Meta_direct_eval",
            "Meta_direct_eval_facts",
            "Meta_direct_result_new",
            "invoke_backend_facts",
            "invoke_program_phase_facts",
            "invoke_procedure_facts",
        ):
            if spelling in bootstrap_text:
                failures.append(f"bootstrap Meta evaluator special case remains: {spelling}")

    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        return 1

    mode = "source-boundary" if "--final" in sys.argv else "migration"
    print(f"PASS lainc -> LAINIR API contract ({mode})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
