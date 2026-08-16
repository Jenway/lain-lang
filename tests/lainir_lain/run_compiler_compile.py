#!/usr/bin/env python3
"""Exercise the first real compiler_compile source-to-LAIN-IR slice."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
BOOTSTRAP = ROOT / "bootstrap" / "zig-out" / "bin"
L1CHECK = BOOTSTRAP / "lainir-print.exe"
L1I = BOOTSTRAP / "lainir-run.exe"
RUNNER = ROOT / "scripts" / "run_lain_compiler.py"
GOOD = pathlib.Path(__file__).parent / "fixtures" / "function_return.lain"
ARITHMETIC = pathlib.Path(__file__).parent / "fixtures" / "function_arithmetic.lain"
CALLS = pathlib.Path(__file__).parent / "fixtures" / "function_calls.lain"
WIDTHS = pathlib.Path(__file__).parent / "fixtures" / "function_widths.lain"
ADDRESS = pathlib.Path(__file__).parent / "fixtures" / "function_address.lain"
QUALIFIED_CALL = pathlib.Path(__file__).parent / "fixtures" / "function_qualified_call.lain"
EFFECTS = pathlib.Path(__file__).parent / "fixtures" / "function_effects.lain"
TYPE_ALIAS = pathlib.Path(__file__).parent / "fixtures" / "function_type_alias.lain"
BOOL = pathlib.Path(__file__).parent / "fixtures" / "function_bool.lain"
TYPE_HELPER = pathlib.Path(__file__).parent / "fixtures" / "type_helper.lain"
EXTERNAL_TYPE_MAIN = pathlib.Path(__file__).parent / "fixtures" / "function_main_type_external.lain"
GENERIC_TYPE_ALIAS = pathlib.Path(__file__).parent / "fixtures" / "function_generic_type_alias.lain"
CALL_EXPRESSION = pathlib.Path(__file__).parent / "fixtures" / "function_call_expression.lain"
COMPARISON = pathlib.Path(__file__).parent / "fixtures" / "function_comparison.lain"
IF_ORDER = pathlib.Path(__file__).parent / "fixtures" / "function_if_order.lain"
QUALIFIED_CONSTANT = pathlib.Path(__file__).parent / "fixtures" / "function_qualified_constant.lain"
STD_IMPORT = pathlib.Path(__file__).parent / "fixtures" / "std_import.lain"
PACKAGE_MAIN = pathlib.Path(__file__).parent / "fixtures" / "package_main.lain"
PACKAGE_HELPER = pathlib.Path(__file__).parent / "fixtures" / "package_helper.lain"
NOMINAL_PARAM = pathlib.Path(__file__).parent / "fixtures" / "function_nominal_param.lain"
IF_FIXTURE = pathlib.Path(__file__).parent / "fixtures" / "function_if.lain"
IF_PARAM = pathlib.Path(__file__).parent / "fixtures" / "function_if_param.lain"
LOCAL = pathlib.Path(__file__).parent / "fixtures" / "function_local.lain"
WHILE = pathlib.Path(__file__).parent / "fixtures" / "function_while.lain"
RECORD_AND_FUNCTION = pathlib.Path(__file__).parent / "fixtures" / "record_and_function.lain"
RECORD_LITERAL_MISSING = pathlib.Path(__file__).parent / "fixtures" / "function_record_literal_missing.lain"
RECORD_LITERAL_UNKNOWN = pathlib.Path(__file__).parent / "fixtures" / "function_record_literal_unknown.lain"
RECORD_LITERAL_DUPLICATE = pathlib.Path(__file__).parent / "fixtures" / "function_record_literal_duplicate.lain"
RECORD_LITERAL_CALL_NESTED = pathlib.Path(__file__).parent / "fixtures" / "function_record_literal_call_nested.lain"
DIVZERO = pathlib.Path(__file__).parent / "fixtures" / "function_divzero.lain"
BAD = pathlib.Path(__file__).parent / "fixtures" / "function_bad.lain"
UNKNOWN_CALL = pathlib.Path(__file__).parent / "fixtures" / "function_unknown_call.lain"
BAD_ARITY = pathlib.Path(__file__).parent / "fixtures" / "function_bad_arity.lain"
HELPER = pathlib.Path(__file__).parent / "fixtures" / "function_helpers.lain"
EXTERNAL_MAIN = pathlib.Path(__file__).parent / "fixtures" / "function_main_external.lain"
CONSTEVAL = pathlib.Path(__file__).parent / "fixtures" / "consteval_and_function.lain"
CONSTEVAL_DIVZERO = pathlib.Path(__file__).parent / "fixtures" / "consteval_compile_divzero.lain"
CONSTEVAL_DUPLICATE = pathlib.Path(__file__).parent / "fixtures" / "consteval_duplicate.lain"
MISSING_IMPORT = pathlib.Path(__file__).parent / "fixtures" / "module_missing.lain"
CYCLE_A = pathlib.Path(__file__).parent / "fixtures" / "module_cycle_a.lain"
CYCLE_B = pathlib.Path(__file__).parent / "fixtures" / "module_cycle_b.lain"
NESTED_MAIN = pathlib.Path(__file__).parent / "fixtures" / "nested_module_main.lain"
MODULE_DUPLICATE_MEMBER_NAMES = pathlib.Path(__file__).parent / "fixtures" / "module_duplicate_member_names.lain"
MODULE_FACTORY_BIND = pathlib.Path(__file__).parent / "fixtures" / "module_factory_bind.lain"
NESTED_PROJECTION = pathlib.Path(__file__).parent / "fixtures" / "nested_projection_call.lain"
UFCS_RECEIVER = pathlib.Path(__file__).parent / "fixtures" / "ufcs_receiver.lain"
UNKNOWN_ATTRIBUTE = pathlib.Path(__file__).parent / "fixtures" / "unknown_attribute.lain"
DANGLING_ATTRIBUTE = pathlib.Path(__file__).parent / "fixtures" / "dangling_attribute.lain"
CONSTEVAL_CALL = pathlib.Path(__file__).parent / "fixtures" / "consteval_call.lain"
CONSTEVAL_IN_FUNCTION = pathlib.Path(__file__).parent / "fixtures" / "consteval_in_function.lain"


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-compiler-compile-") as temporary:
        directory = pathlib.Path(temporary)
        generated = directory / "function.l1"
        compiled = run(
            [sys.executable, str(RUNNER), "-o", str(generated), str(GOOD)]
        )
        if compiled.returncode:
            print(compiled.stderr or compiled.stdout, file=sys.stderr)
            return 1
        checked = run([str(L1CHECK), str(generated), "main"])
        if checked.returncode:
            print(checked.stderr or checked.stdout, file=sys.stderr)
            return 1
        executed = run([str(L1I), str(generated), "main"])
        if executed.returncode or executed.stdout.strip() != "42":
            print(executed.stderr or executed.stdout, file=sys.stderr)
            return 1
        arithmetic_output = directory / "arithmetic.l1"
        arithmetic = run(
            [sys.executable, str(RUNNER), "-o", str(arithmetic_output), str(ARITHMETIC)]
        )
        if arithmetic.returncode:
            print(arithmetic.stderr or arithmetic.stdout, file=sys.stderr)
            return 1
        arithmetic_text = arithmetic_output.read_text(encoding="utf-8")
        if "#add(40, 2)" not in arithmetic_text:
            print("compiler_compile folded away the runtime arithmetic lowering", file=sys.stderr)
            return 1
        checked_arithmetic = run([str(L1CHECK), str(arithmetic_output), "main"])
        executed_arithmetic = run([str(L1I), str(arithmetic_output), "main"])
        if checked_arithmetic.returncode or executed_arithmetic.stdout.strip() != "42":
            print(
                checked_arithmetic.stderr
                or executed_arithmetic.stderr
                or executed_arithmetic.stdout,
                file=sys.stderr,
            )
            return 1
        calls_output = directory / "calls.l1"
        calls = run(
            [sys.executable, str(RUNNER), "-o", str(calls_output), str(CALLS)]
        )
        if calls.returncode:
            print(calls.stderr or calls.stdout, file=sys.stderr)
            return 1
        calls_text = calls_output.read_text(encoding="utf-8")
        if "#call f0_4_0_add(40, 2)" not in calls_text or "#bits<32> %left" not in calls_text:
            print("compiler_compile did not lower the direct function call", file=sys.stderr)
            return 1
        checked_calls = run([str(L1CHECK), str(calls_output), "main"])
        executed_calls = run([str(L1I), str(calls_output), "main"])
        if checked_calls.returncode or executed_calls.stdout.strip() != "42":
            print(
                checked_calls.stderr or executed_calls.stderr or executed_calls.stdout,
                file=sys.stderr,
            )
            return 1
        multi_output = directory / "multi-source.l1"
        multi = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(multi_output),
                str(EXTERNAL_MAIN),
                str(HELPER),
            ]
        )
        if multi.returncode:
            print(multi.stderr or multi.stdout, file=sys.stderr)
            return 1
        multi_text = multi_output.read_text(encoding="utf-8")
        if "#proc main() -> #bits<32>" not in multi_text or "#proc f1_4_0_add(" not in multi_text:
            print("compiler_compile did not lower functions from multiple sources", file=sys.stderr)
            return 1
        checked_multi = run([str(L1CHECK), str(multi_output), "main"])
        executed_multi = run([str(L1I), str(multi_output), "main"])
        if checked_multi.returncode or executed_multi.stdout.strip() != "42":
            print(
                checked_multi.stderr or executed_multi.stderr or executed_multi.stdout,
                file=sys.stderr,
            )
            return 1
        units_output = directory / "syntax-units.txt"
        units = run(
            [
                str(BOOTSTRAP / "lainir-interpreter.exe"),
                str(ROOT / "build" / "lainir" / "lain_compiler.l1"),
                "syntax_units_dump",
                str(units_output),
                str(EXTERNAL_MAIN),
                str(HELPER),
            ]
        )
        if units.returncode or "(syntax-units count=2)" not in units_output.read_text(encoding="utf-8"):
            print(units.stderr or units.stdout, file=sys.stderr)
            return 1
        consteval_output = directory / "consteval.l1"
        consteval = run(
            [sys.executable, str(RUNNER), "-o", str(consteval_output), str(CONSTEVAL)]
        )
        if consteval.returncode:
            print(consteval.stderr or consteval.stdout, file=sys.stderr)
            return 1
        checked_consteval = run([str(L1CHECK), str(consteval_output), "main"])
        executed_consteval = run([str(L1I), str(consteval_output), "main"])
        if checked_consteval.returncode or executed_consteval.stdout.strip() != "42":
            print(
                checked_consteval.stderr
                or executed_consteval.stderr
                or executed_consteval.stdout,
                file=sys.stderr,
            )
            return 1
        consteval_bad_output = directory / "consteval-divzero.l1"
        consteval_bad = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(consteval_bad_output),
                str(CONSTEVAL_DIVZERO),
            ]
        )
        if consteval_bad.returncode == 0 or "3103" not in (consteval_bad.stderr or consteval_bad.stdout):
            print("compiler_compile accepted consteval division by zero", file=sys.stderr)
            return 1
        duplicate_output = directory / "consteval-duplicate.l1"
        duplicate = run(
            [sys.executable, str(RUNNER), "-o", str(duplicate_output), str(CONSTEVAL_DUPLICATE)]
        )
        if duplicate.returncode == 0 or "5113" not in (duplicate.stderr or duplicate.stdout):
            print("compiler_compile accepted a duplicate constant", file=sys.stderr)
            return 1
        missing_output = directory / "missing-import.l1"
        missing = run(
            [sys.executable, str(RUNNER), "-o", str(missing_output), str(MISSING_IMPORT)]
        )
        if missing.returncode == 0 or "4101" not in (missing.stderr or missing.stdout):
            print("compiler_compile accepted an unresolved import", file=sys.stderr)
            return 1
        cycle_output = directory / "cycle-import.l1"
        cycle = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(cycle_output),
                str(CYCLE_A),
                str(CYCLE_B),
            ]
        )
        if cycle.returncode == 0 or "4103" not in (cycle.stderr or cycle.stdout):
            print("compiler_compile accepted an import cycle", file=sys.stderr)
            return 1
        nested_output = directory / "nested-module.l1"
        nested = run(
            [sys.executable, str(RUNNER), "-o", str(nested_output), str(NESTED_MAIN)]
        )
        if nested.returncode:
            print(nested.stderr or nested.stdout, file=sys.stderr)
            return 1
        checked_nested = run([str(L1CHECK), str(nested_output), "main"])
        executed_nested = run([str(L1I), str(nested_output), "main"])
        if checked_nested.returncode or executed_nested.stdout.strip() != "42":
            print(
                checked_nested.stderr
                or executed_nested.stderr
                or executed_nested.stdout,
                file=sys.stderr,
            )
            return 1
        for attribute_source, diagnostic in (
            (UNKNOWN_ATTRIBUTE, "2902"),
            (DANGLING_ATTRIBUTE, "12005"),
        ):
            attribute_output = directory / f"{attribute_source.stem}.l1"
            attribute_run = run(
                [
                    sys.executable,
                    str(RUNNER),
                    "-o",
                    str(attribute_output),
                    str(attribute_source),
                ]
            )
            if attribute_run.returncode == 0 or diagnostic not in (
                attribute_run.stderr or attribute_run.stdout
            ):
                print(
                    f"compiler_compile accepted invalid attribute {attribute_source.name}",
                    file=sys.stderr,
                )
                return 1
        consteval_call_output = directory / "consteval-call.l1"
        consteval_call = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(consteval_call_output),
                str(CONSTEVAL_CALL),
            ]
        )
        if consteval_call.returncode:
            print(consteval_call.stderr or consteval_call.stdout, file=sys.stderr)
            return 1
        consteval_call_text = consteval_call_output.read_text(encoding="utf-8")
        if "#return 42" not in consteval_call_text:
            print("compiler_compile did not execute a consteval function call", file=sys.stderr)
            return 1
        checked_consteval_call = run([str(L1CHECK), str(consteval_call_output), "main"])
        executed_consteval_call = run([str(L1I), str(consteval_call_output), "main"])
        if checked_consteval_call.returncode or executed_consteval_call.stdout.strip() != "42":
            print(
                checked_consteval_call.stderr
                or executed_consteval_call.stderr
                or executed_consteval_call.stdout,
                file=sys.stderr,
            )
            return 1
        consteval_in_function_output = directory / "consteval-in-function.l1"
        consteval_in_function = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(consteval_in_function_output),
                str(CONSTEVAL_IN_FUNCTION),
            ]
        )
        if consteval_in_function.returncode:
            print(
                consteval_in_function.stderr or consteval_in_function.stdout,
                file=sys.stderr,
            )
            return 1
        checked_consteval_in_function = run(
            [str(L1CHECK), str(consteval_in_function_output), "main"]
        )
        executed_consteval_in_function = run(
            [str(L1I), str(consteval_in_function_output), "main"]
        )
        if (
            checked_consteval_in_function.returncode
            or executed_consteval_in_function.stdout.strip() != "42"
        ):
            print(
                checked_consteval_in_function.stderr
                or executed_consteval_in_function.stderr
                or executed_consteval_in_function.stdout,
                file=sys.stderr,
            )
            return 1
        widths_output = directory / "widths.l1"
        widths = run(
            [sys.executable, str(RUNNER), "-o", str(widths_output), str(WIDTHS)]
        )
        if widths.returncode:
            print(widths.stderr or widths.stdout, file=sys.stderr)
            return 1
        widths_text = widths_output.read_text(encoding="utf-8")
        if "#proc f0_4_0_widen(#bits<64> %value) -> #bits<64>" not in widths_text:
            print("compiler_compile lost physical i64 signature width", file=sys.stderr)
            return 1
        checked_widths = run([str(L1CHECK), str(widths_output), "main"])
        if checked_widths.returncode:
            print(checked_widths.stderr or checked_widths.stdout, file=sys.stderr)
            return 1
        address_output = directory / "address.l1"
        address = run(
            [sys.executable, str(RUNNER), "-o", str(address_output), str(ADDRESS)]
        )
        if address.returncode:
            print(address.stderr or address.stdout, file=sys.stderr)
            return 1
        address_text = address_output.read_text(encoding="utf-8")
        if "#proc f0_4_0_identity(addr %value) -> addr" not in address_text:
            print("compiler_compile did not lower the Meta address type", file=sys.stderr)
            return 1
        checked_address = run([str(L1CHECK), str(address_output), "main"])
        executed_address = run([str(L1I), str(address_output), "main"])
        if checked_address.returncode or executed_address.stdout.strip() != "42":
            print(
                checked_address.stderr
                or executed_address.stderr
                or executed_address.stdout,
                file=sys.stderr,
            )
            return 1
        qualified_output = directory / "qualified-call.l1"
        qualified = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(qualified_output),
                str(QUALIFIED_CALL),
            ]
        )
        if qualified.returncode:
            print(qualified.stderr or qualified.stdout, file=sys.stderr)
            return 1
        qualified_text = qualified_output.read_text(encoding="utf-8")
        if not re.search(r"#call f0_41_[0-9]+_add\(40, 2\)", qualified_text):
            print("compiler_compile did not resolve a qualified Meta call", file=sys.stderr)
            return 1
        checked_qualified = run([str(L1CHECK), str(qualified_output), "main"])
        executed_qualified = run([str(L1I), str(qualified_output), "main"])
        if checked_qualified.returncode or executed_qualified.stdout.strip() != "42":
            print(
                checked_qualified.stderr
                or executed_qualified.stderr
                or executed_qualified.stdout,
                file=sys.stderr,
            )
            return 1
        effects_output = directory / "effects.l1"
        effects = run(
            [sys.executable, str(RUNNER), "-o", str(effects_output), str(EFFECTS)]
        )
        if effects.returncode:
            print(effects.stderr or effects.stdout, file=sys.stderr)
            return 1
        effects_text = effects_output.read_text(encoding="utf-8")
        if "#proc f0_4_0_borrow(addr %value) -> addr" not in effects_text:
            print("compiler_compile did not erase the source effect clause at the boundary", file=sys.stderr)
            return 1
        checked_effects = run([str(L1CHECK), str(effects_output), "main"])
        executed_effects = run([str(L1I), str(effects_output), "main"])
        if checked_effects.returncode or executed_effects.stdout.strip() != "42":
            print(
                checked_effects.stderr
                or executed_effects.stderr
                or executed_effects.stdout,
                file=sys.stderr,
            )
            return 1
        alias_output = directory / "type-alias.l1"
        alias = run(
            [sys.executable, str(RUNNER), "-o", str(alias_output), str(TYPE_ALIAS)]
        )
        if alias.returncode:
            print(alias.stderr or alias.stdout, file=sys.stderr)
            return 1
        alias_text = alias_output.read_text(encoding="utf-8")
        if "#proc f0_84_3284500170_widen(#bits<64> %value) -> #bits<64>" not in alias_text:
            print("compiler_compile did not resolve a Meta type alias", file=sys.stderr)
            return 1
        if "#proc f0_150_3224519577_keep(addr %value) -> addr" not in alias_text:
            print("compiler_compile did not lower a record type alias as an address", file=sys.stderr)
            return 1
        checked_alias = run([str(L1CHECK), str(alias_output), "main"])
        executed_alias = run([str(L1I), str(alias_output), "main"])
        if checked_alias.returncode or executed_alias.stdout.strip() != "42":
            print(
                checked_alias.stderr or executed_alias.stderr or executed_alias.stdout,
                file=sys.stderr,
            )
            return 1
        bool_output = directory / "bool.l1"
        bool_run = run(
            [sys.executable, str(RUNNER), "-o", str(bool_output), str(BOOL)]
        )
        if bool_run.returncode:
            print(bool_run.stderr or bool_run.stdout, file=sys.stderr)
            return 1
        bool_text = bool_output.read_text(encoding="utf-8")
        if "#proc f0_4_0_truth() -> #bits<1>" not in bool_text or "#return 1" not in bool_text:
            print("compiler_compile did not lower a Meta bool literal", file=sys.stderr)
            return 1
        checked_bool = run([str(L1CHECK), str(bool_output), "main"])
        executed_bool = run([str(L1I), str(bool_output), "main"])
        if checked_bool.returncode or executed_bool.stdout.strip() != "42":
            print(
                checked_bool.stderr or executed_bool.stderr or executed_bool.stdout,
                file=sys.stderr,
            )
            return 1
        external_type_output = directory / "external-type.l1"
        external_type = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(external_type_output),
                str(EXTERNAL_TYPE_MAIN),
                str(TYPE_HELPER),
            ]
        )
        if external_type.returncode:
            print(external_type.stderr or external_type.stdout, file=sys.stderr)
            return 1
        external_type_text = external_type_output.read_text(encoding="utf-8")
        if "#proc f0_4_5209418473003_widen(#bits<64> %value) -> #bits<64>" not in external_type_text:
            print("compiler_compile did not resolve a type from a later source unit", file=sys.stderr)
            return 1
        checked_external_type = run([str(L1CHECK), str(external_type_output), "main"])
        executed_external_type = run([str(L1I), str(external_type_output), "main"])
        if checked_external_type.returncode or executed_external_type.stdout.strip() != "42":
            print(
                checked_external_type.stderr
                or executed_external_type.stderr
                or executed_external_type.stdout,
                file=sys.stderr,
            )
            return 1
        generic_output = directory / "generic-type-alias.l1"
        generic = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(generic_output),
                str(GENERIC_TYPE_ALIAS),
            ]
        )
        if generic.returncode:
            print(generic.stderr or generic.stdout, file=sys.stderr)
            return 1
        generic_text = generic_output.read_text(encoding="utf-8")
        if not re.search(
            r"#proc f0_84_[0-9]+_keep\(addr %value\) -> addr",
            generic_text,
        ):
            print("compiler_compile did not preserve a Meta generic constructor as an address", file=sys.stderr)
            return 1
        checked_generic = run([str(L1CHECK), str(generic_output), "main"])
        executed_generic = run([str(L1I), str(generic_output), "main"])
        if checked_generic.returncode or executed_generic.stdout.strip() != "42":
            print(
                checked_generic.stderr or executed_generic.stderr or executed_generic.stdout,
                file=sys.stderr,
            )
            return 1
        call_expression_output = directory / "call-expression.l1"
        call_expression = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(call_expression_output),
                str(CALL_EXPRESSION),
            ]
        )
        if call_expression.returncode:
            print(call_expression.stderr or call_expression.stdout, file=sys.stderr)
            return 1
        call_expression_text = call_expression_output.read_text(encoding="utf-8")
        if (
            "#call f0_4_0_add(" not in call_expression_text
            or "#trunc[#bits<32>](#add(20, 20))" not in call_expression_text
            or "#trunc[#bits<32>](#add(1, 1))" not in call_expression_text
        ):
            print("compiler_compile did not lower expressions inside call arguments", file=sys.stderr)
            return 1
        checked_call_expression = run([str(L1CHECK), str(call_expression_output), "main"])
        executed_call_expression = run([str(L1I), str(call_expression_output), "main"])
        if checked_call_expression.returncode or executed_call_expression.stdout.strip() != "42":
            print(
                checked_call_expression.stderr
                or executed_call_expression.stderr
                or executed_call_expression.stdout,
                file=sys.stderr,
            )
            return 1
        comparison_output = directory / "comparison.l1"
        comparison = run(
            [sys.executable, str(RUNNER), "-o", str(comparison_output), str(COMPARISON)]
        )
        if comparison.returncode:
            print(comparison.stderr or comparison.stdout, file=sys.stderr)
            return 1
        comparison_text = comparison_output.read_text(encoding="utf-8")
        if "#return #lt(%left, %right)" not in comparison_text or "#return #eq(%left, %right)" not in comparison_text:
            print("compiler_compile did not lower comparison expressions", file=sys.stderr)
            return 1
        checked_comparison = run([str(L1CHECK), str(comparison_output), "main"])
        executed_comparison = run([str(L1I), str(comparison_output), "main"])
        if checked_comparison.returncode or executed_comparison.stdout.strip() != "42":
            print(
                checked_comparison.stderr or executed_comparison.stderr or executed_comparison.stdout,
                file=sys.stderr,
            )
            return 1
        if_order_output = directory / "if-order.l1"
        if_order = run(
            [sys.executable, str(RUNNER), "-o", str(if_order_output), str(IF_ORDER)]
        )
        if if_order.returncode:
            print(if_order.stderr or if_order.stdout, file=sys.stderr)
            return 1
        if_order_text = if_order_output.read_text(encoding="utf-8")
        if "#if #lt(%value, 1)" not in if_order_text:
            print("compiler_compile did not lower an ordered if comparison", file=sys.stderr)
            return 1
        checked_if_order = run([str(L1CHECK), str(if_order_output), "main"])
        executed_if_order = run([str(L1I), str(if_order_output), "main"])
        if checked_if_order.returncode or executed_if_order.stdout.strip() != "42":
            print(
                checked_if_order.stderr or executed_if_order.stderr or executed_if_order.stdout,
                file=sys.stderr,
            )
            return 1
        qualified_constant_output = directory / "qualified-constant.l1"
        qualified_constant = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(qualified_constant_output),
                str(QUALIFIED_CONSTANT),
            ]
        )
        if qualified_constant.returncode:
            print(qualified_constant.stderr or qualified_constant.stdout, file=sys.stderr)
            return 1
        qualified_constant_text = qualified_constant_output.read_text(encoding="utf-8")
        if "#return 42" not in qualified_constant_text:
            print("compiler_compile did not resolve a qualified Meta constant", file=sys.stderr)
            return 1
        checked_qualified_constant = run([str(L1CHECK), str(qualified_constant_output), "main"])
        executed_qualified_constant = run([str(L1I), str(qualified_constant_output), "main"])
        if checked_qualified_constant.returncode or executed_qualified_constant.stdout.strip() != "42":
            print(
                checked_qualified_constant.stderr
                or executed_qualified_constant.stderr
                or executed_qualified_constant.stdout,
                file=sys.stderr,
            )
            return 1
        std_import_output = directory / "std-import.l1"
        std_import = run(
            [sys.executable, str(RUNNER), "-o", str(std_import_output), str(STD_IMPORT)]
        )
        if std_import.returncode:
            print(std_import.stderr or std_import.stdout, file=sys.stderr)
            return 1
        checked_std_import = run([str(L1CHECK), str(std_import_output), "main"])
        executed_std_import = run([str(L1I), str(std_import_output), "main"])
        if checked_std_import.returncode or executed_std_import.stdout.strip() != "42":
            print(
                checked_std_import.stderr or executed_std_import.stderr or executed_std_import.stdout,
                file=sys.stderr,
            )
            return 1
        package_output = directory / "package-import.l1"
        package = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(package_output),
                str(PACKAGE_MAIN),
                str(PACKAGE_HELPER),
            ]
        )
        if package.returncode:
            print(package.stderr or package.stdout, file=sys.stderr)
            return 1
        checked_package = run([str(L1CHECK), str(package_output), "main"])
        executed_package = run([str(L1I), str(package_output), "main"])
        if checked_package.returncode or executed_package.stdout.strip() != "42":
            print(
                checked_package.stderr or executed_package.stderr or executed_package.stdout,
                file=sys.stderr,
            )
            return 1
        nominal_output = directory / "nominal-param.l1"
        nominal = run(
            [sys.executable, str(RUNNER), "-o", str(nominal_output), str(NOMINAL_PARAM)]
        )
        if nominal.returncode:
            print(nominal.stderr or nominal.stdout, file=sys.stderr)
            return 1
        nominal_text = nominal_output.read_text(encoding="utf-8")
        if not re.search(r"#proc f0_[0-9]+_[0-9]+_f\(addr %Memory\) -> addr", nominal_text):
            print("compiler_compile did not lower nominal qualified types", file=sys.stderr)
            return 1
        checked_nominal = run([str(L1CHECK), str(nominal_output), "main"])
        executed_nominal = run([str(L1I), str(nominal_output), "main"])
        if checked_nominal.returncode or executed_nominal.stdout.strip() != "42":
            print(
                checked_nominal.stderr or executed_nominal.stderr or executed_nominal.stdout,
                file=sys.stderr,
            )
            return 1
        nested_projection_output = directory / "nested-projection.l1"
        nested_projection = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(nested_projection_output),
                str(NESTED_PROJECTION),
            ]
        )
        if nested_projection.returncode:
            print(nested_projection.stderr or nested_projection.stdout, file=sys.stderr)
            return 1
        nested_projection_text = nested_projection_output.read_text(encoding="utf-8")
        if not re.search(
            r"#load\[#bits<32>\]\(#lea\(base=#load\[(?:#addr|addr)\]",
            nested_projection_text,
        ):
            print("compiler_compile did not lower a continuous field projection", file=sys.stderr)
            return 1
        checked_nested_projection = run(
            [str(L1CHECK), str(nested_projection_output), "main"]
        )
        executed_nested_projection = run(
            [str(L1I), str(nested_projection_output), "main"]
        )
        if (
            checked_nested_projection.returncode
            or executed_nested_projection.stdout.strip() != "42"
        ):
            print(
                checked_nested_projection.stderr
                or executed_nested_projection.stderr
                or executed_nested_projection.stdout,
                file=sys.stderr,
            )
            return 1
        ufcs_output = directory / "ufcs-receiver.l1"
        ufcs = run(
            [sys.executable, str(RUNNER), "-o", str(ufcs_output), str(UFCS_RECEIVER)]
        )
        if ufcs.returncode:
            print(ufcs.stderr or ufcs.stdout, file=sys.stderr)
            return 1
        ufcs_text = ufcs_output.read_text(encoding="utf-8")
        if not re.search(
            r"#proc f0_[0-9]+_[0-9]+_get\(addr %value\)", ufcs_text
        ) or not re.search(
            r"#call f0_[0-9]+_[0-9]+_get\(%value\)", ufcs_text
        ):
            print("compiler_compile did not lower the UFCS receiver", file=sys.stderr)
            return 1
        checked_ufcs = run([str(L1CHECK), str(ufcs_output), "main"])
        executed_ufcs = run([str(L1I), str(ufcs_output), "main"])
        if checked_ufcs.returncode or executed_ufcs.stdout.strip() != "42":
            print(
                checked_ufcs.stderr or executed_ufcs.stderr or executed_ufcs.stdout,
                file=sys.stderr,
            )
            return 1
        if_output = directory / "if.l1"
        if_run = run(
            [sys.executable, str(RUNNER), "-o", str(if_output), str(IF_FIXTURE)]
        )
        if if_run.returncode:
            print(if_run.stderr or if_run.stdout, file=sys.stderr)
            return 1
        if_text = if_output.read_text(encoding="utf-8")
        if "#if #eq(1, 1)" not in if_text or "#return 0" not in if_text:
            print("compiler_compile did not lower both branches", file=sys.stderr)
            return 1
        checked_if = run([str(L1CHECK), str(if_output), "main"])
        executed_if = run([str(L1I), str(if_output), "main"])
        if checked_if.returncode or executed_if.stdout.strip() != "42":
            print(checked_if.stderr or executed_if.stderr or executed_if.stdout, file=sys.stderr)
            return 1
        if_param_output = directory / "if-param.l1"
        if_param = run(
            [sys.executable, str(RUNNER), "-o", str(if_param_output), str(IF_PARAM)]
        )
        if if_param.returncode:
            print(if_param.stderr or if_param.stdout, file=sys.stderr)
            return 1
        if_param_text = if_param_output.read_text(encoding="utf-8")
        if "#if #eq(%value, 0)" not in if_param_text:
            print("compiler_compile did not lower a parameter condition", file=sys.stderr)
            return 1
        checked_if_param = run([str(L1CHECK), str(if_param_output), "main"])
        executed_if_param = run([str(L1I), str(if_param_output), "main"])
        if checked_if_param.returncode or executed_if_param.stdout.strip() != "42":
            print(
                checked_if_param.stderr
                or executed_if_param.stderr
                or executed_if_param.stdout,
                file=sys.stderr,
            )
            return 1
        local_output = directory / "local.l1"
        local = run(
            [sys.executable, str(RUNNER), "-o", str(local_output), str(LOCAL)]
        )
        if local.returncode:
            print(local.stderr or local.stdout, file=sys.stderr)
            return 1
        local_text = local_output.read_text(encoding="utf-8")
        if "#let %value: #bits<32> = 40" not in local_text:
            print("compiler_compile did not lower a local binding", file=sys.stderr)
            return 1
        checked_local = run([str(L1CHECK), str(local_output), "main"])
        executed_local = run([str(L1I), str(local_output), "main"])
        if checked_local.returncode or executed_local.stdout.strip() != "42":
            print(checked_local.stderr or executed_local.stderr or executed_local.stdout, file=sys.stderr)
            return 1
        while_output = directory / "while.l1"
        while_run = run(
            [sys.executable, str(RUNNER), "-o", str(while_output), str(WHILE)]
        )
        if while_run.returncode:
            print(while_run.stderr or while_run.stdout, file=sys.stderr)
            return 1
        while_text = while_output.read_text(encoding="utf-8")
        if "#loop loop0" not in while_text or "#break loop0" not in while_text:
            print("compiler_compile did not lower a while loop", file=sys.stderr)
            return 1
        checked_while = run([str(L1CHECK), str(while_output), "main"])
        executed_while = run([str(L1I), str(while_output), "main"])
        if checked_while.returncode or executed_while.stdout.strip() != "3":
            print(checked_while.stderr or executed_while.stderr or executed_while.stdout, file=sys.stderr)
            return 1
        duplicate_module_output = directory / "module-duplicate-members.l1"
        duplicate_module = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(duplicate_module_output),
                str(MODULE_DUPLICATE_MEMBER_NAMES),
            ]
        )
        if duplicate_module.returncode:
            print(duplicate_module.stderr or duplicate_module.stdout, file=sys.stderr)
            return 1
        duplicate_module_text = duplicate_module_output.read_text(encoding="utf-8")
        if not re.search(r"#proc f0_[0-9]+_[0-9]+_add\(", duplicate_module_text):
            print("module member functions did not receive deterministic labels", file=sys.stderr)
            return 1
        checked_duplicate_module = run(
            [str(L1CHECK), str(duplicate_module_output), "main"]
        )
        executed_duplicate_module = run(
            [str(L1I), str(duplicate_module_output), "main"]
        )
        if (
            checked_duplicate_module.returncode
            or executed_duplicate_module.stdout.strip() != "42"
        ):
            print(
                checked_duplicate_module.stderr
                or executed_duplicate_module.stderr
                or executed_duplicate_module.stdout,
                file=sys.stderr,
            )
            return 1
        factory_output = directory / "module-factory-bind.l1"
        factory = run(
            [sys.executable, str(RUNNER), "-o", str(factory_output), str(MODULE_FACTORY_BIND)]
        )
        if factory.returncode:
            print(factory.stderr or factory.stdout, file=sys.stderr)
            return 1
        checked_factory = run([str(L1CHECK), str(factory_output), "main"])
        executed_factory = run([str(L1I), str(factory_output), "main"])
        if checked_factory.returncode or executed_factory.stdout.strip() != "42":
            print(
                checked_factory.stderr or executed_factory.stderr or executed_factory.stdout,
                file=sys.stderr,
            )
            return 1
        record_output = directory / "record-and-function.l1"
        record_run = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(record_output),
                str(RECORD_AND_FUNCTION),
            ]
        )
        if record_run.returncode:
            print(record_run.stderr or record_run.stdout, file=sys.stderr)
            return 1
        checked_record = run([str(L1CHECK), str(record_output), "main"])
        executed_record = run([str(L1I), str(record_output), "main"])
        if checked_record.returncode or executed_record.stdout.strip() != "42":
            print(
                checked_record.stderr or executed_record.stderr or executed_record.stdout,
                file=sys.stderr,
            )
            return 1
        nested_record_output = directory / "record-literal-call-nested.l1"
        nested_record = run(
            [
                sys.executable,
                str(RUNNER),
                "-o",
                str(nested_record_output),
                str(RECORD_LITERAL_CALL_NESTED),
            ]
        )
        if nested_record.returncode:
            print(nested_record.stderr or nested_record.stdout, file=sys.stderr)
            return 1
        checked_nested_record = run([str(L1CHECK), str(nested_record_output), "main"])
        executed_nested_record = run([str(L1I), str(nested_record_output), "main"])
        if (
            checked_nested_record.returncode
            or executed_nested_record.returncode
            or executed_nested_record.stdout.strip() != "41"
        ):
            print(
                checked_nested_record.stderr
                or executed_nested_record.stderr
                or executed_nested_record.stdout,
                file=sys.stderr,
            )
            return 1
        for source in (
            RECORD_LITERAL_MISSING,
            RECORD_LITERAL_UNKNOWN,
            RECORD_LITERAL_DUPLICATE,
        ):
            rejected_record = directory / f"{source.stem}.l1"
            result = run(
                [sys.executable, str(RUNNER), "-o", str(rejected_record), str(source)]
            )
            if result.returncode == 0 or "5108" not in (result.stderr or result.stdout):
                print(
                    f"compiler_compile accepted invalid record literal {source.name}",
                    file=sys.stderr,
                )
                return 1
        bad_output = directory / "bad.l1"
        rejected = run(
            [sys.executable, str(RUNNER), "-o", str(bad_output), str(BAD)]
        )
        if rejected.returncode == 0 or "5111" not in (rejected.stderr or rejected.stdout):
            print("compiler_compile accepted the unsupported parameter form", file=sys.stderr)
            return 1
        if "(error 5111)" not in bad_output.read_text(encoding="utf-8"):
            print("compiler_compile did not preserve its diagnostic artifact", file=sys.stderr)
            return 1
        divzero_output = directory / "divzero.l1"
        divzero = run(
            [sys.executable, str(RUNNER), "-o", str(divzero_output), str(DIVZERO)]
        )
        if divzero.returncode == 0 or "5107" not in (divzero.stderr or divzero.stdout):
            print("compiler_compile accepted compile-time division by zero", file=sys.stderr)
            return 1
        for source, diagnostic in ((UNKNOWN_CALL, "5108"), (BAD_ARITY, "5109")):
            rejected_call = directory / f"{source.stem}.l1"
            result = run(
                [sys.executable, str(RUNNER), "-o", str(rejected_call), str(source)]
            )
            if result.returncode == 0 or diagnostic not in (result.stderr or result.stdout):
                print(f"compiler_compile accepted invalid call {source.name}", file=sys.stderr)
                return 1
    print("PASS compiler_compile function slice")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
