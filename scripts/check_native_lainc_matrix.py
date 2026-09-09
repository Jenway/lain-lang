#!/usr/bin/env python3
"""Run the representative source matrix through a native Lain compiler."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "scripts" / "fixtures"
WORK = ROOT / "build" / "native-lainc-matrix"

SUCCESS_FIXTURES = (
    "formal_constant_return.lain",
    "formal_arithmetic_return.lain",
    "formal_subtraction_return.lain",
    "formal_multiplication_return.lain",
    "formal_division_return.lain",
    "formal_call_return.lain",
    "formal_call_argument_expression.lain",
    "formal_call_argument_return.lain",
    "formal_call_two_arguments_return.lain",
    "formal_call_named_arguments_return.lain",
    "formal_local_binding_return.lain",
    "formal_local_arithmetic_binding_return.lain",
    "formal_local_call_return.lain",
    "formal_two_local_binding_return.lain",
    "formal_two_local_call_return.lain",
    "formal_struct_field_return.lain",
    "formal_struct_two_fields_return.lain",
    "formal_module_call_return.lain",
    "formal_parenthesized_return.lain",
    "formal_unit_return.lain",
)
MULTI_FIXTURE_GROUPS = (
    ("formal_import_main.lain", "formal_import_leaf.lain"),
)
FAILURE_FIXTURES = {
    "empty_source.lain": 5100,
    "formal_ast_conformance.lain": 5102,
    "formal_cycle_a.lain": 4101,
    "formal_cycle_b.lain": 4101,
    "formal_duplicate_module.lain": 3013,
    "formal_duplicate_struct.lain": 3008,
    "formal_if_eq_return.lain": 5106,
    "formal_if_false_return.lain": 5106,
    "formal_if_ne_return.lain": 5106,
    "formal_if_true_return.lain": 5106,
    "formal_if_without_else.lain": 5106,
    "formal_import_compile_leaf.lain": 5100,
    "formal_import_compile_main.lain": 4101,
    "formal_import_leaf.lain": 5100,
    "formal_import_target.lain": 4101,
    "formal_invalid_return.lain": 5108,
    "formal_unknown_procedure.lain": 5108,
    "formal_duplicate_procedure.lain": 5110,
    "formal_call_arity_mismatch.lain": 5109,
    "formal_unknown_local.lain": 5108,
    "formal_missing_return_value.lain": 5106,
    "formal_call_argument_separator.lain": 5106,
    "formal_duplicate_local.lain": 2102,
    "formal_return_unit_call_mismatch.lain": 5108,
    "formal_addr_return_integer_mismatch.lain": 5108,
    "formal_addr_return_local_integer_mismatch.lain": 5108,
    "formal_addr_return_i64_call_mismatch.lain": 5108,
    "formal_i64_return_module_mismatch.lain": 5108,
    "formal_i64_return_record_mismatch.lain": 5108,
    "formal_macro_extra_argument.lain": 5108,
    "formal_macro_hygiene.lain": 5108,
    "formal_macro_missing_argument.lain": 5108,
    "formal_macro_recursive.lain": 5108,
    "formal_macro_return.lain": 5108,
    "formal_macro_two_args_return.lain": 5108,
    "formal_macro_two_declarations_return.lain": 5108,
    "formal_meta_unterminated_comment.lain": 5100,
    "formal_missing_struct_field.lain": 5203,
    "formal_missing_module_member.lain": 5203,
    "formal_missing_type_member.lain": 5203,
    "formal_unresolved_import.lain": 4101,
}


def run(
    compiler: Path, fixtures: tuple[str, ...], output: Path
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(compiler), "-o", str(output), *(str(FIXTURES / fixture) for fixture in fixtures)],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_native_lainc_matrix.py <lainc.exe>", file=sys.stderr)
        return 2
    compiler = Path(sys.argv[1])
    if not compiler.is_absolute():
        compiler = ROOT / compiler
    if not compiler.exists():
        print(f"missing native compiler: {compiler}", file=sys.stderr)
        return 2

    shutil.rmtree(WORK, ignore_errors=True)
    WORK.mkdir(parents=True)

    for fixture in SUCCESS_FIXTURES:
        output = WORK / f"{fixture}.l1"
        result = run(compiler, (fixture,), output)
        if (
            result.returncode != 0
            or not output.exists()
            or output.stat().st_size == 0
            or b"\x00" in output.read_bytes()
        ):
            print(f"FAIL {fixture}: exit={result.returncode}")
            if result.stderr:
                print(result.stderr, file=sys.stderr)
            return 1

    for index, fixtures in enumerate(MULTI_FIXTURE_GROUPS):
        output = WORK / f"multi-{index}.l1"
        result = run(compiler, fixtures, output)
        if (
            result.returncode != 0
            or not output.exists()
            or output.stat().st_size == 0
            or b"\x00" in output.read_bytes()
        ):
            print(f"FAIL {' + '.join(fixtures)}: exit={result.returncode}")
            if result.stderr:
                print(result.stderr, file=sys.stderr)
            return 1

    for fixture, expected in FAILURE_FIXTURES.items():
        output = WORK / f"{fixture}.l1"
        result = run(compiler, (fixture,), output)
        if (
            result.returncode != expected
            or output.exists()
            or output.with_suffix(output.suffix + ".tmp").exists()
        ):
            print(
                f"FAIL atomic failure {fixture}: "
                f"exit={result.returncode} expected={expected}"
            )
            return 1

    print(
        f"PASS native lainc matrix ({len(SUCCESS_FIXTURES)} success, "
        f"{len(MULTI_FIXTURE_GROUPS)} multi-file, "
        f"{len(FAILURE_FIXTURES)} diagnostic failures)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
