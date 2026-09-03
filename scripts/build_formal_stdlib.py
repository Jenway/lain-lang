#!/usr/bin/env python3
"""Build and verify the formal Lain standard-library source closure.

The result is deliberately separate from ``bootstrap_std.l1``.  The latter
is the LAIN-IR bootstrap implementation used by the compiler; this artifact
is the output of compiling the user-facing ``std/**/*.lain`` sources.  The
artifact also checks in the five stable compiler ABI entry names; the entry
functions are a migration boundary until their semantic passes are complete.
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUN_COMPILER = ROOT / "scripts" / "run_lain_compiler.py"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
CORE = ROOT / "build" / "lainir" / "lain_compiler_core.l1"
SEED_PRINT = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-print.exe" if os.name == "nt" else "lainir-print"
)
SEED_RUN = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)
OUTPUT = ROOT / "build" / "lainir" / "formal_stdlib.l1"
MANIFEST = ROOT / "build" / "lainir" / "formal_stdlib.manifest.json"
ABI_PROBE = ROOT / "build" / "lainir" / "formal_stdlib_abi_probe.l1"
ABI_OUTPUT = ROOT / "build" / "lainir" / "formal_stdlib_abi_output.l1"
FORMAL_CONSTANT_PROBE = ROOT / "scripts" / "fixtures" / "formal_constant_return.lain"
FORMAL_ARITHMETIC_PROBE = ROOT / "scripts" / "fixtures" / "formal_arithmetic_return.lain"
FORMAL_CALL_PROBE = ROOT / "scripts" / "fixtures" / "formal_call_return.lain"
FORMAL_CALL_ARGUMENT_PROBE = ROOT / "scripts" / "fixtures" / "formal_call_argument_return.lain"
FORMAL_CALL_TWO_ARGUMENTS_PROBE = ROOT / "scripts" / "fixtures" / "formal_call_two_arguments_return.lain"
FORMAL_CALL_NAMED_ARGUMENTS_PROBE = ROOT / "scripts" / "fixtures" / "formal_call_named_arguments_return.lain"
FORMAL_UNIT_PROBE = ROOT / "scripts" / "fixtures" / "formal_unit_return.lain"
FORMAL_CALL_ARGUMENT_EXPRESSION_PROBE = ROOT / "scripts" / "fixtures" / "formal_call_argument_expression.lain"
FORMAL_IF_PROBES = (
    (ROOT / "scripts" / "fixtures" / "formal_if_true_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_if_false_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_if_eq_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_if_ne_return.lain", "42"),
)
FORMAL_IF_WITHOUT_ELSE_PROBE = ROOT / "scripts" / "fixtures" / "formal_if_without_else.lain"
FORMAL_INVALID_PROBE = ROOT / "scripts" / "fixtures" / "formal_invalid_return.lain"
FORMAL_UNRESOLVED_IMPORT_PROBE = (
    ROOT / "scripts" / "fixtures" / "formal_unresolved_import.lain"
)
FORMAL_RESOLVED_IMPORT_PROBES = (
    ROOT / "scripts" / "fixtures" / "formal_import_main.lain",
    ROOT / "scripts" / "fixtures" / "formal_import_target.lain",
    ROOT / "scripts" / "fixtures" / "formal_import_leaf.lain",
)
FORMAL_EXTRA_ARITHMETIC_PROBES = (
    (ROOT / "scripts" / "fixtures" / "formal_subtraction_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_multiplication_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_division_return.lain", "42"),
)
ABI_ENTRIES = (
    "lain_std_abi_version",
    "lain_std_initialize",
    "lain_std_expand",
    "lain_std_elaborate",
    "lain_std_lower",
)
ABI_SUPPORT_ENTRIES = (
    "syntax_units_build",
    "syntax_index_count",
    "syntax_index_entry",
    "syntax_index_imports",
    "syntax_index_import_status",
)


def run(arguments: list[Path | str]) -> None:
    result = subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def verify_abi_entry() -> None:
    run([sys.executable, BUNDLER, "-o", ABI_PROBE, CORE, OUTPUT])
    result = subprocess.run(
        [str(SEED_RUN), "run", str(ABI_PROBE), "lain_std_abi_version"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode or result.stdout.strip() != "1":
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(
            "formal stdlib ABI version entry failed"
            + (f": {detail}" if detail else "")
        )
    selected = subprocess.run(
        [
            str(SEED_RUN),
            "interpreter",
            str(ABI_PROBE),
            "compiler_compile_library",
            str(ABI_OUTPUT),
            str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    selected_diagnostics = selected.stdout + selected.stderr
    if selected.returncode == 0 or "5203" not in selected_diagnostics:
        raise RuntimeError(
            "formal stdlib replacement did not reach the pending lower pass"
        )
    constant_probe = subprocess.run(
        [
            str(SEED_RUN),
            "interpreter",
            str(ABI_PROBE),
            "compiler_compile_library",
            str(ABI_OUTPUT),
            str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
            str(FORMAL_CONSTANT_PROBE),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if constant_probe.returncode:
        detail = constant_probe.stderr.strip() or constant_probe.stdout.strip()
        raise RuntimeError(
            "formal stdlib constant lowering failed"
            + (f": {detail}" if detail else "")
        )
    constant_run = subprocess.run(
        [str(SEED_RUN), "run", str(ABI_OUTPUT), "main"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if constant_run.returncode or constant_run.stdout.strip() != "42":
        detail = constant_run.stderr.strip() or constant_run.stdout.strip()
        raise RuntimeError(
            "formal stdlib constant artifact did not run as 42"
            + (f": {detail}" if detail else "")
        )
    arithmetic_probe = subprocess.run(
        [
            str(SEED_RUN),
            "interpreter",
            str(ABI_PROBE),
            "compiler_compile_library",
            str(ABI_OUTPUT),
            str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
            str(FORMAL_ARITHMETIC_PROBE),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if arithmetic_probe.returncode:
        detail = arithmetic_probe.stderr.strip() or arithmetic_probe.stdout.strip()
        raise RuntimeError(
            "formal stdlib arithmetic lowering failed"
            + (f": {detail}" if detail else "")
        )
    arithmetic_artifact = ABI_OUTPUT.read_text(encoding="utf-8")
    if "#eval" not in arithmetic_artifact:
        raise RuntimeError(
            "formal stdlib arithmetic lowering did not emit a #eval block"
        )
    arithmetic_run = subprocess.run(
        [str(SEED_RUN), "run", str(ABI_OUTPUT), "main"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if arithmetic_run.returncode or arithmetic_run.stdout.strip() != "42":
        detail = arithmetic_run.stderr.strip() or arithmetic_run.stdout.strip()
        raise RuntimeError(
            "formal stdlib arithmetic artifact did not run as 42"
            + (f": {detail}" if detail else "")
        )
    for arithmetic_fixture, expected in FORMAL_EXTRA_ARITHMETIC_PROBES:
        extra_probe = subprocess.run(
            [
                str(SEED_RUN),
                "interpreter",
                str(ABI_PROBE),
                "compiler_compile_library",
                str(ABI_OUTPUT),
                str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
                str(arithmetic_fixture),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if extra_probe.returncode:
            detail = extra_probe.stderr.strip() or extra_probe.stdout.strip()
            raise RuntimeError(
                f"formal stdlib arithmetic lowering failed for {arithmetic_fixture.name}"
                + (f": {detail}" if detail else "")
            )
        extra_run = subprocess.run(
            [str(SEED_RUN), "run", str(ABI_OUTPUT), "main"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if extra_run.returncode or extra_run.stdout.strip() != expected:
            detail = extra_run.stderr.strip() or extra_run.stdout.strip()
            raise RuntimeError(
                f"formal stdlib arithmetic artifact failed for {arithmetic_fixture.name}"
                + (f": {detail}" if detail else "")
            )
    call_probe = subprocess.run(
        [
            str(SEED_RUN),
            "interpreter",
            str(ABI_PROBE),
            "compiler_compile_library",
            str(ABI_OUTPUT),
            str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
            str(FORMAL_CALL_PROBE),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if call_probe.returncode:
        detail = call_probe.stderr.strip() or call_probe.stdout.strip()
        raise RuntimeError(
            "formal stdlib function-call lowering failed"
            + (f": {detail}" if detail else "")
        )
    call_run = subprocess.run(
        [str(SEED_RUN), "run", str(ABI_OUTPUT), "main"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if call_run.returncode or call_run.stdout.strip() != "40":
        detail = call_run.stderr.strip() or call_run.stdout.strip()
        raise RuntimeError(
            "formal stdlib function-call artifact did not run as 40"
            + (f": {detail}" if detail else "")
        )
    call_argument_probe = subprocess.run(
        [
            str(SEED_RUN),
            "interpreter",
            str(ABI_PROBE),
            "compiler_compile_library",
            str(ABI_OUTPUT),
            str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
            str(FORMAL_CALL_ARGUMENT_PROBE),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if call_argument_probe.returncode:
        detail = call_argument_probe.stderr.strip() or call_argument_probe.stdout.strip()
        raise RuntimeError(
            "formal stdlib argument call lowering failed"
            + (f": {detail}" if detail else "")
        )
    call_argument_run = subprocess.run(
        [str(SEED_RUN), "run", str(ABI_OUTPUT), "main"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if call_argument_run.returncode or call_argument_run.stdout.strip() != "42":
        detail = call_argument_run.stderr.strip() or call_argument_run.stdout.strip()
        raise RuntimeError(
            "formal stdlib argument call artifact did not run as 42"
            + (f": {detail}" if detail else "")
        )
    call_two_probe = subprocess.run(
        [
            str(SEED_RUN),
            "interpreter",
            str(ABI_PROBE),
            "compiler_compile_library",
            str(ABI_OUTPUT),
            str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
            str(FORMAL_CALL_TWO_ARGUMENTS_PROBE),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if call_two_probe.returncode:
        detail = call_two_probe.stderr.strip() or call_two_probe.stdout.strip()
        raise RuntimeError(
            "formal stdlib two-argument call lowering failed"
            + (f": {detail}" if detail else "")
        )
    call_two_run = subprocess.run(
        [str(SEED_RUN), "run", str(ABI_OUTPUT), "main"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if call_two_run.returncode or call_two_run.stdout.strip() != "42":
        detail = call_two_run.stderr.strip() or call_two_run.stdout.strip()
        raise RuntimeError(
            "formal stdlib two-argument call artifact did not run as 42"
            + (f": {detail}" if detail else "")
        )
    named_call_probe = subprocess.run(
        [
            str(SEED_RUN),
            "interpreter",
            str(ABI_PROBE),
            "compiler_compile_library",
            str(ABI_OUTPUT),
            str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
            str(FORMAL_CALL_NAMED_ARGUMENTS_PROBE),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if named_call_probe.returncode:
        detail = named_call_probe.stderr.strip() or named_call_probe.stdout.strip()
        raise RuntimeError(
            "formal stdlib named-parameter call lowering failed"
            + (f": {detail}" if detail else "")
        )
    named_call_run = subprocess.run(
        [str(SEED_RUN), "run", str(ABI_OUTPUT), "main"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if named_call_run.returncode or named_call_run.stdout.strip() != "42":
        detail = named_call_run.stderr.strip() or named_call_run.stdout.strip()
        raise RuntimeError(
            "formal stdlib named-parameter call artifact did not run as 42"
            + (f": {detail}" if detail else "")
        )
    unit_probe = subprocess.run(
        [
            str(SEED_RUN),
            "interpreter",
            str(ABI_PROBE),
            "compiler_compile_library",
            str(ABI_OUTPUT),
            str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
            str(FORMAL_UNIT_PROBE),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if unit_probe.returncode:
        detail = unit_probe.stderr.strip() or unit_probe.stdout.strip()
        raise RuntimeError(
            "formal stdlib unit-return lowering failed"
            + (f": {detail}" if detail else "")
        )
    unit_artifact = ABI_OUTPUT.read_text(encoding="utf-8")
    if "#proc main() -> #unit" not in unit_artifact or "#return\n" not in unit_artifact:
        raise RuntimeError("formal stdlib unit-return artifact was malformed")
    unit_print = subprocess.run(
        [str(SEED_PRINT), str(ABI_OUTPUT)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if unit_print.returncode:
        detail = unit_print.stderr.strip() or unit_print.stdout.strip()
        raise RuntimeError(
            "formal stdlib unit-return artifact failed verification"
            + (f": {detail}" if detail else "")
        )
    invalid_argument_probe = subprocess.run(
        [
            str(SEED_RUN),
            "interpreter",
            str(ABI_PROBE),
            "compiler_compile_library",
            str(ABI_OUTPUT),
            str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
            str(FORMAL_CALL_ARGUMENT_EXPRESSION_PROBE),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    invalid_argument_diagnostics = (
        invalid_argument_probe.stdout + invalid_argument_probe.stderr
    )
    if invalid_argument_probe.returncode == 0 or "5203" not in invalid_argument_diagnostics:
        raise RuntimeError(
            "formal stdlib accepted a non-scalar function argument"
        )
    for if_fixture, expected in FORMAL_IF_PROBES:
        if_probe = subprocess.run(
            [
                str(SEED_RUN),
                "interpreter",
                str(ABI_PROBE),
                "compiler_compile_library",
                str(ABI_OUTPUT),
                str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
                str(if_fixture),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if if_probe.returncode:
            detail = if_probe.stderr.strip() or if_probe.stdout.strip()
            raise RuntimeError(
                f"formal stdlib if lowering failed for {if_fixture.name}"
                + (f": {detail}" if detail else "")
            )
        if_run = subprocess.run(
            [str(SEED_RUN), "run", str(ABI_OUTPUT), "main"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if if_run.returncode or if_run.stdout.strip() != expected:
            detail = if_run.stderr.strip() or if_run.stdout.strip()
            raise RuntimeError(
                f"formal stdlib if artifact failed for {if_fixture.name}"
                + (f": {detail}" if detail else "")
            )
    incomplete_if_probe = subprocess.run(
        [
            str(SEED_RUN),
            "interpreter",
            str(ABI_PROBE),
            "compiler_compile_library",
            str(ABI_OUTPUT),
            str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
            str(FORMAL_IF_WITHOUT_ELSE_PROBE),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    incomplete_if_diagnostics = (
        incomplete_if_probe.stdout + incomplete_if_probe.stderr
    )
    if incomplete_if_probe.returncode == 0 or "5203" not in incomplete_if_diagnostics:
        raise RuntimeError("formal stdlib accepted an if without an else branch")
    invalid_probe = subprocess.run(
        [
            str(SEED_RUN),
            "interpreter",
            str(ABI_PROBE),
            "compiler_compile_library",
            str(ABI_OUTPUT),
            str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
            str(FORMAL_INVALID_PROBE),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    invalid_diagnostics = invalid_probe.stdout + invalid_probe.stderr
    if invalid_probe.returncode == 0 or "5203" not in invalid_diagnostics:
        raise RuntimeError(
            "formal stdlib accepted an incomplete arithmetic return"
        )
    import_probe = subprocess.run(
        [
            str(SEED_RUN),
            "interpreter",
            str(ABI_PROBE),
            "syntax_index_import_status",
            str(ABI_OUTPUT),
            str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
            str(FORMAL_UNRESOLVED_IMPORT_PROBE),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    import_diagnostics = import_probe.stdout + import_probe.stderr
    if import_probe.returncode == 0 or "4101" not in import_diagnostics:
        raise RuntimeError(
            "formal stdlib syntax-index did not report an unresolved import"
        )
    resolved_import_probe = subprocess.run(
        [
            str(SEED_RUN),
            "interpreter",
            str(ABI_PROBE),
            "syntax_index_import_status",
            str(ABI_OUTPUT),
            str(ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"),
            *(str(path) for path in FORMAL_RESOLVED_IMPORT_PROBES),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if resolved_import_probe.returncode:
        detail = (
            resolved_import_probe.stderr.strip()
            or resolved_import_probe.stdout.strip()
        )
        raise RuntimeError(
            "formal stdlib syntax-index rejected a resolved local import"
            + (f": {detail}" if detail else "")
        )


def sources() -> tuple[Path, ...]:
    return tuple(
        sorted(
            (path for path in (ROOT / "std").rglob("*.lain") if path.is_file()),
            key=lambda path: path.relative_to(ROOT).as_posix(),
        )
    )


def source_fingerprint(paths: tuple[Path, ...]) -> str:
    digest = hashlib.sha256(b"lain-formal-stdlib-source-closure-v1")
    for path in paths:
        digest.update(path.relative_to(ROOT).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def write_manifest(paths: tuple[Path, ...], fingerprint: str) -> None:
    MANIFEST.write_text(
        json.dumps(
            {
                "schema": "lain-formal-stdlib-source-closure-v1",
                "artifact": OUTPUT.relative_to(ROOT).as_posix(),
                "artifact_sha256": hashlib.sha256(OUTPUT.read_bytes()).hexdigest(),
                "inputs_sha256": fingerprint,
                "target_independent": True,
                "abi": "lain_std_abi_v1",
                "abi_status": "entry-contract",
                "abi_entries": list(ABI_ENTRIES),
                "support_entries": list(ABI_SUPPORT_ENTRIES),
                "sources": [
                    {
                        "path": path.relative_to(ROOT).as_posix(),
                        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    }
                    for path in paths
                ],
            },
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )


def main() -> int:
    paths = sources()
    if not paths:
        raise RuntimeError("no formal standard-library sources found")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    run(
        [
            sys.executable,
            RUN_COMPILER,
            "--library",
            "-o",
            OUTPUT,
            *paths,
        ]
    )
    run([SEED_PRINT, OUTPUT])
    artifact = OUTPUT.read_text(encoding="utf-8")
    missing = [
        name for name in ABI_ENTRIES + ABI_SUPPORT_ENTRIES
        if f"#proc {name}(" not in artifact
    ]
    if missing:
        raise RuntimeError(
            "formal stdlib ABI entries missing: " + ", ".join(missing)
        )
    verify_abi_entry()
    fingerprint = source_fingerprint(paths)
    write_manifest(paths, fingerprint)
    print(f"{OUTPUT.relative_to(ROOT)}")
    print(f"{MANIFEST.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"formal stdlib build failed: {error}", file=sys.stderr)
        raise SystemExit(1)
