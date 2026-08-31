#!/usr/bin/env python3
"""Verify the compiler-core/bootstrap-stdlib split and its ABI wiring."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD_SCRIPT = ROOT / "scripts" / "build_lain_compiler.py"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
BOOTSTRAP = ROOT / "seed" / "zig-out" / "bin"
SEED = BOOTSTRAP / ("lainir-seed.exe" if os.name == "nt" else "lainir-seed")
PRINT = BOOTSTRAP / ("lainir-print.exe" if os.name == "nt" else "lainir-print")
RUNNER = ROOT / "scripts" / "run_lain_compiler.py"
FIXTURE = ROOT / "tests" / "lainir_lain" / "fixtures" / "function_return.lain"
CORE = ROOT / "build" / "lainir" / "lain_compiler_core.l1"
STD = ROOT / "build" / "lainir" / "bootstrap_std.l1"
STD_MANIFEST = ROOT / "build" / "lainir" / "bootstrap_std.manifest.json"
COMBINED = ROOT / "build" / "lainir" / "lain_compiler.l1"
LOWER_PROGRAM = ROOT / "src" / "lainir" / "lain" / "lower_program.l1"
BOOTSTRAP_FORMS = ROOT / "src" / "lainir" / "bootstrap_std" / "core_forms.l1"


def run(arguments: list[pathlib.Path | str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def require(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"{label} failed: {detail}")


def main() -> int:
    try:
        require(run([sys.executable, BUILD_SCRIPT]), "build_lain_compiler")
        for artifact in (CORE, STD, COMBINED, STD_MANIFEST):
            if not artifact.is_file():
                raise RuntimeError(f"missing split artifact: {artifact}")

        core_text = CORE.read_text(encoding="utf-8")
        std_text = STD.read_text(encoding="utf-8")
        manifest = __import__("json").loads(STD_MANIFEST.read_text(encoding="utf-8"))
        if manifest.get("schema") != "lain-bootstrap-stdlib-v1":
            raise RuntimeError("bootstrap stdlib manifest has the wrong schema")
        if manifest.get("abi") != "lain_std_abi_v1" or manifest.get("abi_version") != 1:
            raise RuntimeError("bootstrap stdlib manifest has the wrong ABI")
        if manifest.get("target_independent") is not True:
            raise RuntimeError("bootstrap stdlib manifest lost target-independent marker")
        if not manifest.get("sources"):
            raise RuntimeError("bootstrap stdlib manifest has no source hashes")
        combined_text = COMBINED.read_text(encoding="utf-8")
        lower_program_text = LOWER_PROGRAM.read_text(encoding="utf-8")
        forms_text = BOOTSTRAP_FORMS.read_text(encoding="utf-8")
        if "#extern #proc lain_std_initialize(addr %context)" not in core_text:
            raise RuntimeError("compiler core lost the bootstrap stdlib declaration")
        if "\n#proc lain_std_initialize(addr %context)" in core_text:
            raise RuntimeError("compiler core unexpectedly defines bootstrap stdlib")
        if "#proc lain_std_initialize(addr %context)" not in std_text:
            raise RuntimeError("bootstrap stdlib does not define its ABI entry")
        if combined_text.count("#proc lain_std_initialize(addr %context)") != 1:
            raise RuntimeError("combined compiler has an invalid bootstrap stdlib definition count")
        if "#proc program_meta_status(" in lower_program_text:
            raise RuntimeError("module/struct status policy still has a core implementation")
        if "#extern #proc lain_std_meta_status" not in lower_program_text:
            raise RuntimeError("compiler core lost the stdlib-owned module/struct status hook")
        if "#proc lain_std_meta_status(" not in forms_text:
            raise RuntimeError("bootstrap stdlib does not own the module/struct status hook")
        for moved in (
            "\n#proc module_group(",
            "\n#proc program_module_group(",
            "\n#proc meta_module_build(",
            "\n#proc program_is_module_declaration(",
            "\n#proc program_record_field_for_node(",
            "\n#proc program_type_width(",
            "\n#proc program_eval_consteval_group(",
            "\n#proc program_collect_module(",
        ):
            if moved in core_text:
                raise RuntimeError(f"compiler core still contains moved module semantic: {moved}")
            if moved not in std_text:
                raise RuntimeError(f"bootstrap stdlib lost moved module semantic: {moved}")
        if "#eval {" not in std_text:
            raise RuntimeError("bootstrap stdlib evaluator does not cross the #eval boundary")

        require(run([PRINT, CORE, "compiler_compile"]), "core verification")
        require(run([PRINT, STD, "lain_std_abi_version"]), "stdlib verification")
        require(run([PRINT, COMBINED, "compiler_compile"]), "combined verification")
        std_result = run([SEED, "run", STD, "lain_std_abi_version"])
        require(std_result, "stdlib execution")
        if std_result.stdout.strip() != "1":
            raise RuntimeError(
                f"bootstrap stdlib ABI returned {std_result.stdout.strip()!r}, expected '1'"
            )

        # A mismatched ABI must fail before source lowering. This is the
        # negative half of the bootstrap contract and prevents silent linkage
        # against an incompatible first-generation standard library.
        with tempfile.TemporaryDirectory(prefix="lain-abi-negative-") as temporary:
            directory = pathlib.Path(temporary)
            fake_std = directory / "fake_std.l1"
            fake_std.write_text(
                "#proc lain_std_abi_version() -> i64 { #return 2 }\n"
                "#proc lain_std_initialize(addr %context) -> i32 { #return 0 }\n",
                encoding="utf-8",
            )
            fake_bundle = directory / "fake_bundle.l1"
            require(
                run([sys.executable, BUNDLER, "-o", fake_bundle, CORE, fake_std]),
                "ABI-negative bundle",
            )
            require(run([PRINT, fake_bundle, "compiler_compile"]), "ABI-negative verification")
            output = directory / "rejected.l1"
            rejected = run([SEED, fake_bundle, "compiler_compile", output, FIXTURE])
            if rejected.returncode == 0:
                raise RuntimeError("compiler accepted an incompatible bootstrap stdlib ABI")

        # With a compatible version, a standard-library pass result must still
        # control compilation. This catches accidental fallback to the core's
        # old direct lowering path.
        with tempfile.TemporaryDirectory(prefix="lain-stdlib-delegation-") as temporary:
            directory = pathlib.Path(temporary)
            rejecting_std = directory / "rejecting_std.l1"
            rejecting_std.write_text(
                "#proc lain_std_abi_version() -> i64 { #return 1 }\n"
                "#proc lain_std_initialize(addr %context) -> i32 { #return 0 }\n"
                "#proc lain_std_expand(addr %context, addr %source_unit, addr %root) -> addr {\n"
                "  #return #call lain_meta_pass_result_v1_new(0, 0, %root, #call lain_ast_v1_nil(), #call lain_ast_v1_nil(), %context)\n"
                "}\n"
                "#proc lain_std_elaborate(addr %context, addr %expanded_root) -> addr {\n"
                "  #return #call lain_meta_pass_result_v1_new(0, 0, %expanded_root, #call lain_ast_v1_nil(), #call lain_ast_v1_nil(), %context)\n"
                "}\n"
                "#proc lain_std_lower(addr %context, addr %elaborated_root, addr %l1_unit) -> addr {\n"
                "  #return #call lain_meta_pass_result_v1_new(5992, 1, %l1_unit, #call lain_ast_v1_nil(), #call lain_ast_v1_nil(), %context)\n"
                "}\n",
                encoding="utf-8",
            )
            rejecting_bundle = directory / "rejecting_bundle.l1"
            require(
                run([sys.executable, BUNDLER, "-o", rejecting_bundle, CORE, rejecting_std]),
                "stdlib-delegation bundle",
            )
            require(
                run([PRINT, rejecting_bundle, "compiler_compile"]),
                "stdlib-delegation verification",
            )
            output = directory / "rejected-by-stdlib.l1"
            rejected = run([SEED, rejecting_bundle, "compiler_compile", output, FIXTURE])
            if rejected.returncode == 0:
                raise RuntimeError("compiler ignored the standard-library lower result")

        # Compile a tiny client against the versioned AST wrappers. The client
        # only sees lain_ast_v1_* names, so a RawAst layout change is caught
        # without depending on the compiler's internal procedure names.
        with tempfile.TemporaryDirectory(prefix="lain-ast-api-") as temporary:
            directory = pathlib.Path(temporary)
            probe = directory / "ast_api_probe.l1"
            probe.write_text(
                "#proc lain_ast_api_v1_probe() -> i64 {\n"
                "  #let %node: addr = #call lain_ast_v1_new_atom(4, 7, 1)\n"
                "  #return #add(\n"
                "    #zext[#bits<64>](#call lain_ast_v1_node_kind(%node)),\n"
                "    #call lain_ast_v1_node_length(%node)\n"
                "  )\n"
                "}\n"
                "#proc lain_meta_abi_v1_probe() -> i64 {\n"
                "  #let %nil: addr = #call lain_ast_v1_nil()\n"
                "  #let %result: addr = #call lain_meta_pass_result_v1_new(7, 1, %nil, %nil, %nil, %nil)\n"
                "  #return #add(\n"
                "    #zext[#bits<64>](#call lain_meta_pass_result_v1_status(%result)),\n"
                "    #zext[#bits<64>](#call lain_meta_pass_result_v1_changed(%result))\n"
                "  )\n"
                "}\n"
                "#proc lain_compile_context_v1_probe() -> i64 {\n"
                "  #let %ctx: addr = #call lain_compile_context_v1_new(#call lain_ast_v1_nil(), 2, 3, 4, 5)\n"
                "  #let %s1: i64 = #zext[#bits<64>](#call lain_compile_context_v1_consume_step(%ctx))\n"
                "  #let %s2: i64 = #zext[#bits<64>](#call lain_compile_context_v1_consume_step(%ctx))\n"
                "  #let %s3: i64 = #zext[#bits<64>](#call lain_compile_context_v1_consume_step(%ctx))\n"
                "  #let %b1: i64 = #zext[#bits<64>](#call lain_compile_context_v1_charge_bytes(%ctx, 3))\n"
                "  #let %b2: i64 = #zext[#bits<64>](#call lain_compile_context_v1_charge_bytes(%ctx, 1))\n"
                "  #let %r1: i64 = #zext[#bits<64>](#call lain_compile_context_v1_enter_recursion(%ctx))\n"
                "  #let %sum0: i64 = #call lain_compile_context_v1_step_limit(%ctx)\n"
                "  #let %sum1: i64 = #add(%sum0, #call lain_compile_context_v1_allocation_limit(%ctx))\n"
                "  #let %sum2: i64 = #add(%sum1, #call lain_compile_context_v1_recursion_limit(%ctx))\n"
                "  #let %sum3: i64 = #add(%sum2, #call lain_compile_context_v1_capability_mask(%ctx))\n"
                "  #let %sum4: i64 = #add(%sum3, %s1)\n"
                "  #let %sum5: i64 = #add(%sum4, %s2)\n"
                "  #let %sum6: i64 = #add(%sum5, %s3)\n"
                "  #let %sum7: i64 = #add(%sum6, %b1)\n"
                "  #let %sum8: i64 = #add(%sum7, %b2)\n"
                "  #return #add(%sum8, %r1)\n"
                "}\n",
                encoding="utf-8",
            )
            api_bundle = directory / "ast_api_bundle.l1"
            require(
                run(
                    [
                        sys.executable,
                        BUNDLER,
                        "-o",
                        api_bundle,
                        ROOT / "src" / "lainir" / "tools" / "source.l1",
                        ROOT / "src" / "lainir" / "lain" / "raw_ast.l1",
                        ROOT / "src" / "lainir" / "lain" / "ast_runtime.l1",
                        ROOT / "src" / "lainir" / "lain" / "compiler_context.l1",
                        probe,
                    ]
                ),
                "AstApi v1 bundle",
            )
            require(
                run([PRINT, api_bundle, "lain_ast_api_v1_probe"]),
                "AstApi v1 verification",
            )
            api_result = run([SEED, "run", api_bundle, "lain_ast_api_v1_probe"])
            require(api_result, "AstApi v1 execution")
            if api_result.stdout.strip() != "8":
                raise RuntimeError(
                    f"AstApi v1 probe returned {api_result.stdout.strip()!r}, expected '8'"
                )
            require(
                run([PRINT, api_bundle, "lain_meta_abi_v1_probe"]),
                "MetaPassResult v1 verification",
            )
            meta_result = run([SEED, "run", api_bundle, "lain_meta_abi_v1_probe"])
            require(meta_result, "MetaPassResult v1 execution")
            if meta_result.stdout.strip() != "8":
                raise RuntimeError(
                    "MetaPassResult v1 probe returned "
                    f"{meta_result.stdout.strip()!r}, expected '8'"
                )

            require(
                run([PRINT, api_bundle, "lain_compile_context_v1_probe"]),
                "CompileContext v1 verification",
            )
            context_result = run([SEED, "run", api_bundle, "lain_compile_context_v1_probe"])
            require(context_result, "CompileContext v1 execution")
            if context_result.stdout.strip() != "18":
                raise RuntimeError(
                    "CompileContext v1 probe returned "
                    f"{context_result.stdout.strip()!r}, expected '18'"
                )

        with tempfile.TemporaryDirectory(prefix="lain-bootstrap-stdlib-") as temporary:
            output = pathlib.Path(temporary) / "function.l1"
            require(
                run([sys.executable, RUNNER, "-o", output, FIXTURE]),
                "compiler invocation through linked stdlib",
            )
            require(run([PRINT, output, "main"]), "compiled fixture verification")
            executed = run([SEED, "run", output, "main"])
            require(executed, "compiled fixture execution")
            if executed.stdout.strip() != "42":
                raise RuntimeError(
                    f"compiled fixture returned {executed.stdout.strip()!r}, expected '42'"
                )
    except RuntimeError as error:
        print(str(error), file=sys.stderr)
        return 1
    print("Bootstrap stdlib split: core, ABI, and linked execution checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
