#!/usr/bin/env python3
"""Check the source and public-API boundary between LAINIR and LAINVM."""

from __future__ import annotations

from pathlib import Path

from lainc_sources import lainir_api_sources, lainvm_sources


ROOT = Path(__file__).resolve().parents[1]
INTERPRETER = ROOT / "src" / "lainvm" / "interpreter.lain"
CONTRACT = ROOT / "src" / "lainvm" / "api_contract.lain"
PROVIDER = ROOT / "src" / "lainir" / "api" / "default_provider.lain"
OLD_INTERPRETER = ROOT / "src" / "lainir" / "api" / "l1_interpreter.lain"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"LAINVM boundary: {message}")


def main() -> int:
    api_sources = {path.resolve() for path in lainir_api_sources(ROOT)}
    vm_sources = {path.resolve() for path in lainvm_sources(ROOT)}
    require(INTERPRETER.resolve() in vm_sources, "VM manifest omits interpreter")
    require(CONTRACT.resolve() in vm_sources, "VM manifest omits API contract")
    require(INTERPRETER.resolve() not in api_sources, "LAINIR manifest owns interpreter")
    require(not OLD_INTERPRETER.exists(), "old LAINIR interpreter still exists")

    provider = PROVIDER.read_text(encoding="utf-8")
    require("lainvm::" not in provider, "LAINIR provider imports LAINVM")
    require("@export let Eval" not in provider, "LAINIR provider exports Eval")
    require("ProviderWithExternalCalls" not in provider,
            "LAINIR provider owns VM external-call policy")

    interpreter = INTERPRETER.read_text(encoding="utf-8")
    require("@export\nlet lainvm: Module" in interpreter,
            "LAINVM module export is missing")
    require("let Flow: type" in interpreter,
            "interpreter-private control flow is missing")
    require("@export let Flow" not in interpreter,
            "interpreter-private Flow is public")
    require("@export let Result" not in interpreter,
            "legacy public result wrapper remains")
    require("@export let execute = execute" in interpreter,
            "public VM execute entry is missing")
    require("@export let execute_limited = execute_limited" in interpreter,
            "public limited VM execute entry is missing")
    require("@export let execute_child = execute_child" in interpreter,
            "public child-TCB execution entry is missing")
    require("let run_child = std::func(" in interpreter,
            "child-TCB implementation is missing")
    require(") -> Value ! {\n            Memory.Allocation.Effect,\n"
            "            Memory.Bounds.Effect,\n            effects.Trap," in interpreter,
            "VM execution does not expose Value-or-Trap semantics",
    )
    require("let Eval: effects.Effect = std::effect(\"LAINVM.Eval\", L1)"
            in interpreter, "VM Eval effect is missing")
    require("let eval_handler = std::func(" in interpreter,
            "VM Eval handler is missing")
    require("resume execute_child(" in interpreter,
            "VM Eval handler does not execute a child TCB")
    contract = CONTRACT.read_text(encoding="utf-8")
    for member in (
        "let ExecutionShape: ModuleShape",
        "let Eval: effects.Effect",
        "let new_arguments = std::func() -> ValueVector",
        "let eval = std::func(",
    ):
        require(member in contract, f"VM API contract is missing {member}")
    print("PASS LAINIR/LAINVM source and Value-or-Trap API boundary")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
