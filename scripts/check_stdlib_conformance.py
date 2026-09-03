#!/usr/bin/env python3
"""Compare the bootstrap and formal stdlib on their shared scalar slice."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
CORE = ROOT / "build" / "lainir" / "lain_compiler_core.l1"
BOOTSTRAP_STDLIB = ROOT / "build" / "lainir" / "bootstrap_std.l1"
FORMAL_STDLIB = ROOT / "build" / "lainir" / "formal_stdlib.l1"
SEED = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)
API = ROOT / "src" / "lainir" / "lain" / "compiler_api.l1"
FIXTURES = (
    (ROOT / "scripts" / "fixtures" / "formal_constant_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_arithmetic_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_call_return.lain", "40"),
    (ROOT / "scripts" / "fixtures" / "formal_call_argument_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_call_two_arguments_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_call_named_arguments_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_call_argument_expression.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_parenthesized_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_local_binding_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_local_arithmetic_binding_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_local_call_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_two_local_binding_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_two_local_call_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_module_call_return.lain", "42"),
    (ROOT / "scripts" / "fixtures" / "formal_struct_field_return.lain", "42"),
)
RESOLVED_IMPORT_SOURCES = (
    ROOT / "scripts" / "fixtures" / "formal_import_compile_main.lain",
    ROOT / "scripts" / "fixtures" / "formal_import_compile_leaf.lain",
)
IMPORT_DIAGNOSTIC_CASES = (
    (
        "formal_unresolved_import",
        (ROOT / "scripts" / "fixtures" / "formal_unresolved_import.lain",),
        4101,
    ),
    (
        "formal_import_cycle",
        (
            ROOT / "scripts" / "fixtures" / "formal_cycle_a.lain",
            ROOT / "scripts" / "fixtures" / "formal_cycle_b.lain",
        ),
        4103,
    ),
)


def run(command: list[str], *, cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True)


def canonical(text: str) -> str:
    # The bootstrap compiler uses deterministic fN_source_hash_ prefixes for
    # local procedures; those prefixes are not part of the semantic ABI.
    normalized = re.sub(r"f\d+_[0-9]+_[0-9]+_", "", text)
    # Formal scalar arithmetic deliberately goes through #eval.  Collapse
    # that transparent scalar wrapper before comparing the generated IR.
    normalized = re.sub(
        r"#let %value: #bits<64> = #eval \{ #return ([^\n]+) \}\n"
        r"  #return %value",
        r"#return \1",
        normalized,
    )
    # Formal argument lowering materializes a compile-time argument in a
    # local before the call.  Treat that transparent #eval temporary like the
    # direct expression emitted by the bootstrap implementation.
    normalized = re.sub(
        r"#let %argument_value: #bits<64> = #eval \{ #return ([^\n]+) \}\n"
        r"  #return #call ([^\(]+)\(%argument_value\)",
        r"#return #call \2(\1)",
        normalized,
    )
    # Formal local initializers may materialize arithmetic through #eval;
    # collapse that transparent temporary to the direct expression.
    normalized = re.sub(
        r"(#let %[A-Za-z_][A-Za-z0-9_]*: #bits<64> = )#eval \{ #return ([^\n]+) \}",
        r"\1\2",
        normalized,
    )
    return normalized.strip() + "\n"


def compile_sources(
    compiler: Path,
    sources: tuple[Path, ...],
    output: Path,
) -> subprocess.CompletedProcess[str]:
    result = run(
        [
            str(SEED),
            "interpreter",
            str(compiler),
            "compiler_compile_library",
            str(output),
            str(API),
            *(str(source) for source in sources),
        ]
    )
    return result


def compile_with(compiler: Path, fixture: Path, output: Path) -> None:
    result = compile_sources(compiler, (fixture,), output)
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"{fixture.name}: compiler failed: {detail}")


def compile_status(
    compiler: Path,
    sources: tuple[Path, ...],
    output: Path,
) -> int:
    result = compile_sources(compiler, sources, output)
    if result.returncode == 0:
        return 0
    diagnostics = result.stdout + result.stderr
    match = re.search(r"returned status (\d+)", diagnostics)
    if match is None:
        detail = diagnostics.strip() or "no diagnostic"
        raise RuntimeError(
            f"{sources[0].name}: compiler failed without a status: {detail}"
        )
    return int(match.group(1))


def main() -> int:
    required = (CORE, BOOTSTRAP_STDLIB, FORMAL_STDLIB, SEED, API)
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        print("stdlib conformance: missing artifacts: " + ", ".join(missing), file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="lain-stdlib-conformance-") as directory:
        work = Path(directory)
        bootstrap_compiler = work / "bootstrap_compiler.l1"
        formal_compiler = work / "formal_compiler.l1"
        for compiler, stdlib in (
            (bootstrap_compiler, BOOTSTRAP_STDLIB),
            (formal_compiler, FORMAL_STDLIB),
        ):
            bundled = run([sys.executable, str(BUNDLER), "-o", str(compiler), str(CORE), str(stdlib)])
            if bundled.returncode:
                detail = bundled.stderr.strip() or bundled.stdout.strip()
                print(f"stdlib conformance: failed to bundle {stdlib.name}: {detail}", file=sys.stderr)
                return 1
        for fixture, expected in FIXTURES:
            bootstrap_output = work / f"bootstrap_{fixture.stem}.l1"
            formal_output = work / f"formal_{fixture.stem}.l1"
            compile_with(bootstrap_compiler, fixture, bootstrap_output)
            compile_with(formal_compiler, fixture, formal_output)
            if canonical(bootstrap_output.read_text(encoding="utf-8")) != canonical(
                formal_output.read_text(encoding="utf-8")
            ):
                raise RuntimeError(f"{fixture.name}: generated LAIN-IR differs")
            for label, output in (("bootstrap", bootstrap_output), ("formal", formal_output)):
                executed = run([str(SEED), "run", str(output), "main"])
                if executed.returncode or executed.stdout.strip() != expected:
                    detail = executed.stderr.strip() or executed.stdout.strip()
                    raise RuntimeError(
                        f"{fixture.name}: {label} result was not {expected}: {detail}"
                    )
            print(f"PASS {fixture.stem}: IR and result {expected}")

        bootstrap_import_output = work / "bootstrap_formal_import_compile.l1"
        formal_import_output = work / "formal_formal_import_compile.l1"
        bootstrap_import_compile = compile_sources(
            bootstrap_compiler,
            RESOLVED_IMPORT_SOURCES,
            bootstrap_import_output,
        )
        formal_import_compile = compile_sources(
            formal_compiler,
            RESOLVED_IMPORT_SOURCES,
            formal_import_output,
        )
        if bootstrap_import_compile.returncode or formal_import_compile.returncode:
            bootstrap_detail = (
                bootstrap_import_compile.stderr.strip()
                or bootstrap_import_compile.stdout.strip()
            )
            formal_detail = (
                formal_import_compile.stderr.strip()
                or formal_import_compile.stdout.strip()
            )
            raise RuntimeError(
                "resolved import unexpectedly failed: "
                f"bootstrap={bootstrap_detail}, formal={formal_detail}"
            )
        if canonical(
            bootstrap_import_output.read_text(encoding="utf-8")
        ) != canonical(formal_import_output.read_text(encoding="utf-8")):
            raise RuntimeError("resolved import: generated LAIN-IR differs")
        print("PASS formal_resolved_import: IR matches")

        for name, sources, expected in IMPORT_DIAGNOSTIC_CASES:
            bootstrap_status = compile_status(
                bootstrap_compiler,
                sources,
                work / f"bootstrap_{name}.l1",
            )
            formal_status = compile_status(
                formal_compiler,
                sources,
                work / f"formal_{name}.l1",
            )
            if bootstrap_status != expected or formal_status != expected:
                raise RuntimeError(
                    f"{name}: expected {expected}, got "
                    f"bootstrap={bootstrap_status}, formal={formal_status}"
                )
            print(f"PASS {name}: diagnostic {expected}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL stdlib conformance: {error}", file=sys.stderr)
        raise SystemExit(1)
