#!/usr/bin/env python3
"""Check the LAINIR/LAINVM contract boundary from the lainc side.

The Lain-written LAINVM interpreter and the Lain-written LAINIR provider are
paused and archived under docs/history/formal-implementations/ until Lain is
mature enough to rewrite them.  Their contracts stay in src/, because that is
what a provider must satisfy and what lainc compiles against.

This gate therefore no longer inspects an implementation's shape.  It checks
the invariants that must survive the pause:

* the execution contract is present and still declares its surface;
* lainc depends on the VM contract only, never on a VM implementation;
* Meta asks the VM for compile-time execution instead of evaluating itself;
* the removed evaluation-result protocol has not come back.
"""

from __future__ import annotations

from pathlib import Path

from lainc_sources import lainir_api_sources, lainvm_sources


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "src" / "lainvm" / "api_contract.lain"
LAINIR_CONTRACT = ROOT / "src" / "lainir" / "api_contract.lain"
LAINC = ROOT / "src" / "lainc"
# Files that must never return: the archived implementation must not be
# reintroduced alongside the contract it implements.
ARCHIVED = (
    ROOT / "src" / "lainvm" / "interpreter.lain",
    ROOT / "src" / "lainir" / "api" / "default_provider.lain",
    ROOT / "src" / "lainir" / "api" / "l1_ir.lain",
    ROOT / "src" / "lainir" / "api" / "l1_unit_builder.lain",
    ROOT / "src" / "lainir" / "api" / "l1_verifier.lain",
    ROOT / "src" / "lainir" / "api" / "l1_printer.lain",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"LAINVM boundary: {message}")


def main() -> int:
    api_sources = {path.resolve() for path in lainir_api_sources(ROOT)}
    vm_sources = {path.resolve() for path in lainvm_sources(ROOT)}
    require(CONTRACT.resolve() in vm_sources, "VM manifest omits the execution contract")
    require(
        LAINIR_CONTRACT.resolve() in api_sources,
        "LAINIR manifest omits the capability contract",
    )
    for path in ARCHIVED:
        require(
            not path.exists(),
            f"archived implementation is back in src/: {path.relative_to(ROOT)}",
        )

    contract = CONTRACT.read_text(encoding="utf-8")
    for member in (
        "let ExecutionShape: ModuleShape",
        "let Allocation: effects.Effect",
        "let Eval: effects.Effect",
        "let new_arguments = std::func() -> ValueVector",
        "let append_argument = std::func(",
        "let eval = std::func(",
    ):
        require(member in contract, f"VM execution contract is missing {member}")

    factory_calls = {
        "compiler.lain": "compiler_core.Compiler(Memory, Ir, Vm)",
        "compiler_api.lain": "compiler_core.Compiler(Memory, Ir, Vm)",
        "compiler_driver.lain": "compiler_core.Compiler(Memory, Ir, Vm)",
        "compiler_core.lain": "meta.Meta(Memory, Ir, Vm)",
    }
    for name in (*factory_calls, "meta.lain"):
        source = (LAINC / name).read_text(encoding="utf-8")
        require(
            "packages::lain::lainvm::api_contract" in source,
            f"lainc factory {name} does not import the VM contract",
        )
        require(
            "Vm: lainvm_api.ExecutionShape" in source,
            f"lainc factory {name} does not accept the VM capability",
        )
        require(
            "packages::lain::lainvm::interpreter" not in source,
            f"lainc factory {name} imports a VM implementation",
        )
        require(
            "packages::lain::lainir::api::" not in source,
            f"lainc factory {name} imports a LAINIR provider implementation",
        )
        if name in factory_calls:
            require(
                factory_calls[name] in source,
                f"lainc factory {name} does not forward the VM capability",
            )

    meta = (LAINC / "meta.lain").read_text(encoding="utf-8")
    require(
        "perform Vm.eval(" in meta,
        "Meta does not request compile-time execution through LAINVM",
    )
    require("Ir.Eval" not in meta, "Meta still uses the removed LAINIR evaluator")
    require(
        "Eval.Result" not in meta and "EvalResult" not in meta,
        "Meta still uses an evaluation result wrapper",
    )
    print("PASS LAINIR/LAINVM contract boundary and Value-or-Trap API")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
