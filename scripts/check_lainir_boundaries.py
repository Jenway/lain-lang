#!/usr/bin/env python3
"""Check the compiler-core/bootstrap-stdlib boundary.

The core artifact may expose ABI entry points, but it must not contain the
implementation of language-level semantic passes.  This is intentionally a
small source/artifact check: it catches accidental reintroduction of the old
monolithic evaluator or advanced-form name tests without pretending to prove
the whole compiler architecture.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "build" / "bootstrap" / "compiler_core.l1"
STDLIB = ROOT / "build" / "bootstrap" / "stdlib.l1"

# These names are implementation procedures, not generic ABI accessors.
FORBIDDEN_PROC = re.compile(
    r"^#proc (?:meta_|program_|lower_|eval_|workspace_|syntax_|lain_macro_|ast_\w*macro|ast_arg_expr_end|ast_substitute_params|lain_ast_all_probe)"
)

# Semantic spelling must be interpreted by the standard library.  Keep this
# list restricted to quoted source literals so comments and ABI identifiers do
# not create false positives.
FORBIDDEN_LITERAL = re.compile(
    r'"(?:func|struct|module|import|effect|generic|bounds|record|macro)"'
)


def main() -> int:
    if not CORE.is_file() or not STDLIB.is_file():
        print("boundary check: compiler artifacts are missing", file=sys.stderr)
        return 2

    core_lines = CORE.read_text(encoding="utf-8").splitlines()
    proc_violations = [
        f"{index}: {line.strip()}"
        for index, line in enumerate(core_lines, 1)
        if FORBIDDEN_PROC.search(line)
    ]
    literal_violations = [
        f"{index}: {line.strip()}"
        for index, line in enumerate(core_lines, 1)
        if FORBIDDEN_LITERAL.search(line)
    ]
    if proc_violations or literal_violations:
        print("boundary check: compiler core contains semantic implementation", file=sys.stderr)
        for violation in proc_violations + literal_violations:
            print(violation, file=sys.stderr)
        return 1

    stdlib_text = STDLIB.read_text(encoding="utf-8")
    required = (
        "#proc lain_std_abi_version",
        "#proc lain_std_expand",
        "#proc lain_std_elaborate",
        "#proc lain_std_lower",
        "#proc lain_std_lower_program",
    )
    missing = [name for name in required if name not in stdlib_text]
    if missing:
        print("boundary check: bootstrap stdlib is missing:", ", ".join(missing), file=sys.stderr)
        return 1
    if "lain_core_lower_program" in stdlib_text:
        print(
            "boundary check: bootstrap stdlib still exposes the core-owned lowering name",
            file=sys.stderr,
        )
        return 1
    forbidden_eval_protocol = (
        "EvalResult",
        "eval_result_",
        "#proc eval_group",
        "#proc eval_consteval_group",
        "#proc program_eval_consteval_group",
    )
    present = [name for name in forbidden_eval_protocol if name in stdlib_text]
    if present:
        print(
            "boundary check: bootstrap stdlib contains a removed eval protocol: "
            + ", ".join(present),
            file=sys.stderr,
        )
        return 1

    print("PASS compiler-core/bootstrap-stdlib boundary")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
