#!/usr/bin/env python3
"""Check the source-level lainc -> LAINIR capability boundary."""

from __future__ import annotations

import re
from pathlib import Path

from lainc_sources import compiler_source_names, lainir_api_sources


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "src" / "lainir" / "api_contract.lain"
# The migration roadmap was consolidated into the single active roadmap.  Keep
# this source-boundary gate coupled to that document instead of resurrecting a
# deleted roadmap filename.
ROADMAP = ROOT / "docs" / "roadmaps" / "lain-roadmap.md"
COMPILER_API = ROOT / "src" / "lainc" / "compiler_api.lain"
LEGACY_MODULES = (
    "src/lainc/l1_ir.lain",
    "src/lainc/l1_unit_builder.lain",
    "src/lainc/l1_verifier.lain",
    "src/lainc/l1_printer.lain",
    "src/lainc/l1_interpreter.lain",
)
REQUIRED_SHAPES = ("BuilderShape", "ArtifactShape", "BackendShape")
REQUIRED_COMPILER_DIAGNOSTIC_ACCESSORS = (
    "diagnostic_count",
    "diagnostic_code",
    "diagnostic_source_id",
    "diagnostic_start",
    "diagnostic_end",
    "diagnostic_message",
)
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
    "Capabilities",
    "signed_divide",
    "unsigned_divide",
    "zero_extend",
    "sign_extend",
)
REQUIRED_PROVIDER_BUILDER = (
    "new_unit", "empty_artifact", "discard", "bits_type", "float_type", "addr_type",
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
REQUIRED_BACKEND = (
    "source_count", "source_data", "source_length", "allocate", "copy_bytes",
    "artifact_begin", "artifact_write_byte", "artifact_finish",
)
BUILDER_SURFACE = frozenset(
    REQUIRED_PROVIDER_BUILDER
    + ("Unit", "Artifact", "Type", "Value", "Procedure", "Region")
)
ARTIFACT_SURFACE = frozenset(
    REQUIRED_PROVIDER_ARTIFACT + ("Artifact", "Procedure", "Diagnostic")
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
    for name in REQUIRED_BACKEND:
        if not re.search(rf"\blet\s+{re.escape(name)}\b", contract_text):
            failures.append(f"backend contract is missing {name}")

    if not ROADMAP.is_file():
        failures.append("missing API migration roadmap")

    compiler_api_text = (
        COMPILER_API.read_text(encoding="utf-8") if COMPILER_API.is_file() else ""
    )
    if not compiler_api_text:
        failures.append("missing src/lainc/compiler_api.lain")
    for name in REQUIRED_COMPILER_DIAGNOSTIC_ACCESSORS:
        if not re.search(rf"\blet\s+{re.escape(name)}\b", compiler_api_text):
            failures.append(f"compiler API is missing {name}")
    if not re.search(r"schema_version\s*=\s*\n?\s*std::func\(\)\s*->\s*i32\s*\{\s*return\s+3;", compiler_api_text):
        failures.append("compiler API schema version is not 3")

    provider_path = ROOT / "src" / "lainir" / "api" / "default_provider.lain"
    provider_text = provider_path.read_text(encoding="utf-8") if provider_path.is_file() else ""
    for name in (
        REQUIRED_PROVIDER_BUILDER
        + REQUIRED_PROVIDER_ARTIFACT
    ):
        if not re.search(rf"\blet\s+{re.escape(name)}\b", provider_text):
            failures.append(f"default provider is missing {name}")

    sources = compiler_source_names(ROOT)
    if "src/lainc/backend_c.lain" in sources:
        failures.append(
            "legacy Lain-written backend must remain outside compiler source closure"
        )
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
            if re.search(r"\barchive_[A-Za-z0-9_]*", text):
                failures.append(
                    f"archive-named Meta transition symbol remains: {path.relative_to(ROOT)}"
                )
            if re.search(r'compiler::l1_(?:ir|unit_builder|verifier|printer|interpreter)', text):
                failures.append(f"concrete LAINIR import remains: {path.relative_to(ROOT)}")
            if re.search(r"\bIr\.(?:L1|Access|Interpreter)\b", text):
                failures.append(f"legacy provider surface remains: {path.relative_to(ROOT)}")
            if "DirectUnitFacts" in text:
                failures.append(f"direct unit inspection remains: {path.relative_to(ROOT)}")
            # Every capability member used by active compiler sources must be
            # named by the frozen v1 contract.  This prevents a
            # provider-specific convenience member from silently becoming an
            # API dependency.
            for pattern, surface, label in (
                (r"\b(?:Ir\.)?Builder\.([A-Za-z_][A-Za-z0-9_]*)", BUILDER_SURFACE, "Builder"),
                (r"\bArtifactApi\.([A-Za-z_][A-Za-z0-9_]*)", ARTIFACT_SURFACE, "Artifact"),
            ):
                for member in re.findall(pattern, text):
                    if member not in surface:
                        failures.append(
                            f"contract-undeclared {label} member {member}: "
                            f"{path.relative_to(ROOT)}"
                        )

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
            r"\bIr\.Eval\b",
            r"\bEvalResult\b",
            r"\bEval\.(?:Result|make_result|result_status|result_value)\b",
        ):
            if re.search(pattern, meta_text):
                failures.append(
                    "Meta still depends on the removed LAINIR Eval result interface"
                )
                break

        elaborator_text = (ROOT / "src" / "lainc" / "elaborator.lain").read_text(
            encoding="utf-8"
        )
        core_text = (ROOT / "src" / "lainc" / "compiler_core.lain").read_text(
            encoding="utf-8"
        )
        for required, label in (
            ("let SourceResult: type", "elaborator does not carry source diagnostic nodes"),
            ("node: import_call", "unresolved import does not retain its syntax node"),
            ("diagnostic_source_id: usize", "elaborator does not carry a source id separately from syntax unit"),
        ):
            if required not in elaborator_text:
                failures.append(label)
        for required, label in (
            ("Syntax.node_span_start", "compiler core does not read diagnostic start span"),
            ("Syntax.node_span_end", "compiler core does not read diagnostic end span"),
        ):
            if required not in core_text:
                failures.append(label)

    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        return 1

    mode = "source-boundary" if "--final" in sys.argv else "migration"
    print(f"PASS lainc -> LAINIR API contract ({mode})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
