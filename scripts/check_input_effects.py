#!/usr/bin/env python3
"""Guard `?{...}` input rows: call-site resolution, binding, and diagnostics."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / "scripts" / "run_lain_compiler.py"
BUNDLE = ROOT / "build" / "bootstrap" / "lainc.l1"
SEED = seed_exe("lainir-seed")
FIXTURES = ROOT / "scripts" / "fixtures"
EMPTY_SOURCE = FIXTURES / "empty_source.lain"

# Positives compile *and run*: the callable body uses the resolved input, so the
# printed value is the proof that the call environment supplied it.  In every
# fixture the callee is declared where the name does not exist (at the top
# level, or inside another module) while the call site sits inside a module that
# binds it, so the value cannot reach the body any other way.
POSITIVE = (
    ("explicit type", "formal_input_effect_explicit_type.lain", "41"),
    # The roadmap's strategy row (`?{T, ord: Ord(T)}`) cannot be expressed yet:
    # nothing binds `T`, and the library has no `Ord` to name.  This fixture
    # keeps the shape this phase can verify -- a Module strategy input that the
    # body reads through the returned module, plus a second scalar input the row
    # still demands.
    ("input row maximum", "formal_input_effect_max.lain", "41"),
    # A scalar Meta call carries each input entry as one more physical argument
    # of the temporary LAINIR artifact, so the returned value is the caller's
    # and not a literal the callee could have written.  The callee is declared
    # at the top level, where `local` does not exist, and the call site sits in
    # a module that binds it.
    ("scalar input value", "formal_input_scalar_value.lain", "41"),
    # Explicit parameters come first and the row's entries follow in row order,
    # on both sides of the artifact: the callee reads `second` (100) added to
    # the explicit `base` (1), so a reversal or a dropped entry cannot print
    # 101.
    ("scalar input order", "formal_input_scalar_order.lain", "101"),
    # Every entry becomes a parameter whatever its declared kind, so a Module
    # entry the body never reads still occupies its slot ahead of the scalar
    # entry the body returns (41): a dropped or reordered parameter would
    # desynchronise the artifact from its argument list.
    ("scalar inputs beside a Module entry",
     "formal_input_scalar_mixed.lain", "41"),
    # A scalar input's declared *width* is part of the requirement, not just
    # its kind: the same call whose caller declares `i64` satisfies an `i64`
    # input, while a caller whose declaration determines no width stays
    # accepted because there is nothing to compare (neither side is guessed).
    ("scalar input width match", "formal_input_scalar_width_match.lain", "41"),
    (
        "scalar input width unknown",
        "formal_input_scalar_width_unknown.lain",
        "41",
    ),
    # A parameter type may name an input row entry, and the type value the call
    # site supplies for that entry is then the parameter's type.  Both fixtures
    # run the same callee with the same argument (300) and differ only in the
    # caller's `T`, so the printed value is decided by the resolved width: `i8`
    # truncates the argument to 44 while `i32` carries it whole.
    ("dependent parameter type i8", "formal_input_dependent_i8.lain", "44"),
    ("dependent parameter type i32", "formal_input_dependent_i32.lain", "300"),
    # The same resolution applies to the return annotation alone: `pick` names
    # `T` nowhere else, so only the return position can give the bare entry its
    # kind, and only the caller's type value can give it its width.
    ("dependent return type", "formal_input_dependent_return.lain", "44"),
    # Two more positions can give a bare entry its kind.  The row itself: a
    # peer entry's declared type spells `Vec(T)`, so `T` sits in a type
    # position even though the signature never mentions it.  The body: the
    # explicit annotation of a `let` declaration (`let probe: T = 0;`) is a
    # type position of its own.  Each fixture names `T` in exactly one of those
    # positions, so a derivation that reads only the signature would report
    # 5104 instead of compiling.
    ("input row peer type", "formal_input_row_type.lain", "41"),
    ("body type annotation", "formal_input_body_type.lain", "41"),
)

# Negatives must fail to compile with the listed status, and the validation span
# must point at the declaration token that carries the failure: the entry name
# for an input the call site cannot supply, the declared type for a value that
# does not satisfy it.  The span offset is derived from the row text so a
# diagnostic that blames the body instead of the requirement is caught.
NEGATIVE = (
    (
        "missing input",
        "formal_input_effect_missing.lain",
        5108,
        "local",
        "local: i32",
    ),
    (
        "uninferred input",
        "formal_input_effect_uninferred.lain",
        5104,
        "local",
        "local",
    ),
    (
        "input type mismatch",
        "formal_input_effect_type_mismatch.lain",
        5108,
        "Module",
        "local: Module",
    ),
    # The scalar Meta path reports the same three failures with the same codes
    # and spans: the entry name for an input the call site cannot supply or
    # cannot type, the declared type for a value that does not satisfy it.
    (
        "scalar missing input",
        "formal_input_scalar_missing.lain",
        5108,
        "local",
        "local: i32",
    ),
    (
        "scalar uninferred input",
        "formal_input_scalar_uninferred.lain",
        5104,
        "local",
        "local",
    ),
    (
        "scalar input type mismatch",
        "formal_input_scalar_type_mismatch.lain",
        5108,
        "Module",
        "local: Module",
    ),
    # A scalar of the wrong width does not satisfy a scalar input: `i32` is a
    # scalar, but it is not the declared `i64`, and the failure blames the
    # declared type in the row exactly like a kind mismatch.
    (
        "scalar input width mismatch",
        "formal_input_scalar_width_mismatch.lain",
        5108,
        "i64",
        "local: i64",
    ),
    # A bare entry is only derivable when a type position uses it.  This row
    # names `T` nowhere in the signature even though the call site does supply
    # a type value for it, so the entry stays unconstrained and is reported
    # instead of being guessed from the supplied value.
    (
        "dependent type unused entry",
        "formal_input_dependent_unused.lain",
        5104,
        "T",
        "T",
    ),
    # Only a *type expression* constrains an entry.  This row's peer entry is
    # typed (`bias: i32`), but that type mentions no entry name, and `T`
    # appears in the row as nothing but its own entry, so `T` stays
    # unconstrained: a derivation that counted a peer entry's own name, or the
    # entry itself, would accept the row instead of reporting it.
    (
        "input row peer type unrelated",
        "formal_input_row_type_unrelated.lain",
        5104,
        "T",
        "bias: i32, T",
    ),
)

# Cases no checked-in fixture covers: the row may be omitted or empty, a bare
# `?{name}` is still rejected when it *is* supplied, the resolved value is the
# caller's value and not a constant, the lookup is by exact name in the call
# environment, and the input row stays independent of the `!{}` output row.
CONTROL_POSITIVE = (
    (
        "omitted row",
        "let build = std::func() -> Module {\n"
        "    return std::module {\n"
        "        @export let value: i32 = 41;\n"
        "    };\n"
        "};\n"
        "\n"
        "let built = build();\n"
        "\n"
        "let main = std::func() -> i32 {\n"
        "    return built.value;\n"
        "};\n",
        "41",
    ),
    (
        "empty row",
        "let build = std::func() ?{} -> Module {\n"
        "    return std::module {\n"
        "        @export let value: i32 = 41;\n"
        "    };\n"
        "};\n"
        "\n"
        "let built = build();\n"
        "\n"
        "let main = std::func() -> i32 {\n"
        "    return built.value;\n"
        "};\n",
        "41",
    ),
    (
        "caller value, not a constant",
        "let build = std::func() ?{local: i32} -> Module {\n"
        "    return std::module {\n"
        "        @export let value: i32 = local;\n"
        "    };\n"
        "};\n"
        "\n"
        "let outer: Module = std::module {\n"
        "    let local: i32 = 7;\n"
        "    @export let built = build();\n"
        "};\n"
        "\n"
        "let main = std::func() -> i32 {\n"
        "    return outer.built.value;\n"
        "};\n",
        "7",
    ),
    (
        "input row and effect row stay independent",
        "let build = std::func() ?{local: i32} -> Module !{IO} {\n"
        "    return std::module {\n"
        "        @export let value: i32 = local;\n"
        "    };\n"
        "};\n"
        "\n"
        "let outer: Module = std::module {\n"
        "    let local: i32 = 41;\n"
        "    @export let built = build();\n"
        "};\n"
        "\n"
        "let main = std::func() -> i32 {\n"
        "    return outer.built.value;\n"
        "};\n",
        "41",
    ),
    (
        # The same call whose scalar input changes with the caller's local, so
        # the printed value cannot be a constant the callee supplied.
        "scalar caller value, not a constant",
        "let bias = std::func() ?{local: i32} -> i32 {\n"
        "    return local;\n"
        "};\n"
        "\n"
        "let outer: Module = std::module {\n"
        "    let local: i32 = 7;\n"
        "    @export let answer = bias();\n"
        "};\n"
        "\n"
        "let main = std::func() -> i32 {\n"
        "    return outer.answer;\n"
        "};\n",
        "7",
    ),
)


def dependent_width_source(type_name: str) -> str:
    """The proof program with the caller's `T` set to `type_name`."""
    return (
        "let idf = std::func(a: T) ?{T} -> T {\n"
        "    return a;\n"
        "};\n"
        "\n"
        "let outer: Module = std::module {\n"
        f"    let T: std::type = {type_name};\n"
        "    @export let got: i64 = idf(4294967297);\n"
        "};\n"
        "\n"
        "let main = std::func() -> i64 {\n"
        "    return outer.got;\n"
        "};\n"
    )


# The physical width a dependent annotation takes is the width of the type value
# the call site supplies.  Both programs call the same callee with the same
# argument (2**32 + 1) and differ only in the caller's `T`: the argument is
# appended at the resolved width, so the constant the artifact path folds into
# `main` is the truncated 1 for `i32` and the whole value for `i64`.  `run`
# prints only the low 32 bits, so the emitted LAINIR is the evidence here.
WIDTH_PROOF = (
    ("dependent width i32", dependent_width_source("i32"), "1"),
    ("dependent width i64", dependent_width_source("i64"), "4294967297"),
)

CONTROL_NEGATIVE = (
    (
        # An input is a requirement of the declaration, not of its uses: a row
        # entry the body never reads must still be supplied.
        "unused declared input",
        "let build = std::func() ?{unused: i32} -> Module {\n"
        "    return std::module {\n"
        "        @export let value: i32 = 41;\n"
        "    };\n"
        "};\n"
        "\n"
        "let built = build();\n"
        "\n"
        "let main = std::func() -> i32 {\n"
        "    return built.value;\n"
        "};\n",
        5108,
        "unused",
        "unused: i32",
    ),
    (
        # Exact-name lookup in the call environment: the module supplies
        # `other`, not `local`, so the row stays unsatisfied.
        "caller binding renamed",
        "let build = std::func() ?{local: i32} -> Module {\n"
        "    return std::module {\n"
        "        @export let value: i32 = local;\n"
        "    };\n"
        "};\n"
        "\n"
        "let outer: Module = std::module {\n"
        "    let other: i32 = 41;\n"
        "    @export let built = build();\n"
        "};\n"
        "\n"
        "let main = std::func() -> i32 {\n"
        "    return outer.built.value;\n"
        "};\n",
        5108,
        "local",
        "local: i32",
    ),
)

ERROR_PREFIX = "(error "
NODE_PREFIX = "(context node "


def run(arguments: list[Path | str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def row_token_offset(text: str, row: str, token: str) -> int:
    """Absolute offset of `token` inside the one `?{row}` occurrence."""
    row_start = text.index("?{" + row) + 2
    return row_start + row.index(token)


def read_error(artifact: Path) -> tuple[int, str, int] | None:
    """The reported status, offending token, and its source offset."""
    if not artifact.is_file():
        return None
    status: int | None = None
    node: str | None = None
    start: int | None = None
    for line in artifact.read_text(encoding="utf-8").splitlines():
        if line.startswith(ERROR_PREFIX):
            status = int(line[len(ERROR_PREFIX) :].rstrip(")"))
        elif line.startswith(NODE_PREFIX):
            body = line[len(NODE_PREFIX) :]
            node = body.split(" start=")[0]
            start = int(body.split("start=")[1].split(" ")[0])
    if status is None or node is None or start is None:
        return None
    return status, node, start


def main_return_constant(text: str) -> str | None:
    """The constant the compiler folded into `main`, or None when absent."""
    start = text.find("#proc main()")
    if start < 0:
        return None
    returned = text.find("#return ", start)
    if returned < 0:
        return None
    return text[returned + len("#return ") :].split("\n", 1)[0].strip()


def compile_library(artifact: Path, source: Path) -> subprocess.CompletedProcess[str]:
    return run([sys.executable, COMPILER, "--library", "-o", artifact, source])


def compile_to_error(
    artifact: Path, source: Path
) -> tuple[int, str, int] | None:
    compiled = run(
        [SEED, BUNDLE, "compiler_compile_library", artifact, source, EMPTY_SOURCE]
    )
    if compiled.returncode == 0:
        return None
    return read_error(artifact)


def check_diagnostic(
    work: Path,
    index: int,
    label: str,
    text: str,
    source: Path,
    status: int,
    token: str,
    row: str,
) -> bool:
    artifact = work / f"diagnostic_{index}.l1"
    report = compile_to_error(artifact, source)
    if report is None:
        print(f"{label}: expected a compilation failure, got none", file=sys.stderr)
        return False
    actual, node, start = report
    if actual != status:
        print(
            f"{label}: expected bootstrap status {status}, got {actual}",
            file=sys.stderr,
        )
        return False
    if node != token or text[start : start + len(token)] != token:
        print(
            f"{label}: validation span {node!r} at {start} is not {token!r}",
            file=sys.stderr,
        )
        return False
    expected_start = row_token_offset(text, row, token)
    if start != expected_start:
        print(
            f"{label}: validation span points at {start}, not at the "
            f"declaration token {expected_start}",
            file=sys.stderr,
        )
        return False
    print(f"PASS {label}: diagnostic {status} at `{token}`")
    return True


def main() -> int:
    required = [BUNDLE, EMPTY_SOURCE]
    required.extend(
        FIXTURES / fixture for _, fixture, *_ in POSITIVE + NEGATIVE
    )
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        print(
            f"input effects: missing artifacts: {', '.join(missing)}",
            file=sys.stderr,
        )
        return 2
    with tempfile.TemporaryDirectory(prefix="lain-input-effects-") as temp:
        directory = Path(temp)
        for index, (label, fixture, expected) in enumerate(POSITIVE):
            source = FIXTURES / fixture
            artifact = directory / f"positive_{index}.l1"
            compiled = compile_library(artifact, source)
            if compiled.returncode or not artifact.is_file():
                print(compiled.stderr or compiled.stdout, file=sys.stderr)
                print(f"{label}: fixture did not compile", file=sys.stderr)
                return 1
            executed = run([SEED, "run", artifact, "main"])
            actual = executed.stdout.strip()
            if executed.returncode or actual != expected:
                detail = executed.stderr.strip() or actual or "no output"
                print(f"{label}: result was not {expected}: {detail}", file=sys.stderr)
                return 1
            print(f"PASS {label}: input row resolved from the call site, {actual}")
        for index, (label, source_text, expected) in enumerate(CONTROL_POSITIVE):
            source = directory / f"control_positive_{index}.lain"
            source.write_text(source_text, encoding="utf-8")
            artifact = directory / f"control_positive_{index}.l1"
            compiled = compile_library(artifact, source)
            if compiled.returncode or not artifact.is_file():
                print(compiled.stderr or compiled.stdout, file=sys.stderr)
                print(f"{label}: control did not compile", file=sys.stderr)
                return 1
            executed = run([SEED, "run", artifact, "main"])
            actual = executed.stdout.strip()
            if executed.returncode or actual != expected:
                detail = executed.stderr.strip() or actual or "no output"
                print(f"{label}: result was not {expected}: {detail}", file=sys.stderr)
                return 1
            print(f"PASS {label}: {actual}")
        for index, (label, source_text, expected) in enumerate(WIDTH_PROOF):
            source = directory / f"width_proof_{index}.lain"
            source.write_text(source_text, encoding="utf-8")
            artifact = directory / f"width_proof_{index}.l1"
            compiled = compile_library(artifact, source)
            if compiled.returncode or not artifact.is_file():
                print(compiled.stderr or compiled.stdout, file=sys.stderr)
                print(f"{label}: proof did not compile", file=sys.stderr)
                return 1
            actual = main_return_constant(
                artifact.read_text(encoding="utf-8")
            )
            if actual != expected:
                print(
                    f"{label}: artifact folded {actual}, not {expected}",
                    file=sys.stderr,
                )
                return 1
            print(f"PASS {label}: artifact parameter width folded {actual}")
        for index, (label, fixture, status, token, row) in enumerate(NEGATIVE):
            source = FIXTURES / fixture
            if not check_diagnostic(
                directory,
                index,
                label,
                source.read_text(encoding="utf-8"),
                source,
                status,
                token,
                row,
            ):
                return 1
        for index, (label, source_text, status, token, row) in enumerate(
            CONTROL_NEGATIVE
        ):
            source = directory / f"control_negative_{index}.lain"
            source.write_text(source_text, encoding="utf-8")
            if not check_diagnostic(
                directory,
                100 + index,
                label,
                source_text,
                source,
                status,
                token,
                row,
            ):
                return 1
    print(
        "PASS typed input effects: call-site resolution, binding, dependent "
        "type widths, and the "
        f"{len(NEGATIVE) + len(CONTROL_NEGATIVE)} failure diagnostics"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
