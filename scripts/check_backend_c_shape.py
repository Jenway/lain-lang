#!/usr/bin/env python3
"""Check that the Lain-written C backend emits structurally sound C.

The backend is line oriented: emit_line recognises a construct by its line
prefix.  A branch that matches a prefix and returns without consuming the rest
of the line silently drops everything after it.  That is exactly how
`} else { #return #call f() }` lost its body and its closing brace, leaving a
function one brace short so the next function nested inside it.  The backend
exited 0 throughout, and the only symptom was zig cc cascading
"function definition is not allowed here" over the generated file.

This gate therefore inspects the emitted text directly, which reports the
defect at its source rather than 20 compiler errors later:

* braces balance,
* no top-level function definition appears inside another function's body, and
* each case's expected type spellings and `unsupported L1` markers are present
  (and their wrong predecessors absent).

The last point covers two defects whose only symptom was plausible C: a type
outside the emitter's known set fell back to `uint64_t`, and an L1 expression
directive the emitter did not implement was copied into the output verbatim,
which is invalid C with no trace of the unimplemented construct.

Braces inside string literals, character literals and comments are stripped
first; counting them raw gives a wrong answer for generated code, which is full
of brace characters inside string literals.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
SEED = seed_exe("lainir-seed")
BACKEND = ROOT / "build" / "backend_c_entry.l1"
FIXTURES = ROOT / "scripts" / "fixtures"
# The inline-else fixture is the regression case this check was written for.
# The data fixture is the regression case for `#data`/#`data_addr`: a stray
# pointer-typed initializer or an unescaped data literal breaks the emitted C
# shape before zig cc ever sees it.
#
# Each case lists the C fragments it must and must not contain.  The shape of
# a case's own source makes its type spellings deterministic, so asserting
# them here keeps the defects that only showed up as plausible C -- a wrong
# fallback type, and an unimplemented L1 expression -- visible at the backend
# boundary.  An unimplemented *expression* is no longer one of them: the
# backend refuses it, so its case belongs to FAILING_CASES.
CASES = (
    ("backend_inline_else.l1", (), ()),
    ("backend_constant_return.l1", (), ()),
    ("backend_data_addr.l1", (), ()),
    # `#bits<8>/<16>`, `#float<32>/<64>` and `#never` used to fall back to
    # `uint64_t`; the seed emitter fails on a type it cannot spell, so this
    # backend must not substitute a plausible one either.
    (
        "backend_narrow_types.l1",
        (
            "int8_t byte_out(uintptr_t a);",
            "int16_t short_out(uintptr_t b);",
            "float float_out(uintptr_t c);",
            "double double_out(uintptr_t d);",
            "int8_t local_byte(void);",
            "void die(void);",
        ),
        (
            "uint64_t byte_out(uintptr_t a);",
            "uint64_t short_out(uintptr_t b);",
            "uint64_t float_out(uintptr_t c);",
            "uint64_t double_out(uintptr_t d);",
            "uint64_t local_byte(void);",
        ),
    ),
    # `#call_indirect` shares the `#call` prefix; its bracketed signature used
    # to be copied through as `_indirect[(...) -> ...](...)`, which is not C.
    (
        "backend_call_indirect.l1",
        ("/* unsupported L1: #call_indirect */",),
        ("_indirect[",),
    ),
    # A load or store used to widen to 64 bits whenever its declared width
    # was not 8 (load) or 8/32 (store): `#bits<16>` and `#bits<32>` loads and
    # the `#bits<16>` store emitted the 64-bit helper.  A 16-bit store then
    # overwrote the six bytes after its destination and a 16- or 32-bit load
    # read them.  The required fragments pin each width to its own helper and
    # the forbidden ones are the widened emissions this fixture regressed.
    (
        "backend_load_store_widths.l1",
        (
            "static uint16_t L1_load16(uintptr_t p){",
            "static uint32_t L1_load32(uintptr_t p){",
            "static void L1_store16(uintptr_t p, uint16_t v){",
            # The helpers read through memcpy rather than dereferencing a cast
            # pointer, because `#alloca` produces a byte array and a 32-bit
            # access at an odd offset is therefore unaligned.  Asserting the
            # memcpy keeps a future edit from reintroducing the direct
            # dereference, which panics under a runtime that checks alignment.
            "memcpy(&v, (const void *)p, 2)",
            "memcpy((void *)p, &v, 2)",
            "int16_t b=  L1_load16(p);",
            "L1_store16( p, b);",
            "uint32_t c=  L1_load32(p);",
            "L1_store32( p, c);",
        ),
        (
            "int16_t b=  L1_load64(p);",
            "L1_store64( p, b);",
            "uint32_t c=  L1_load64(p);",
        ),
    ),
)

# Modules this backend cannot lower must be refused: the run has to fail and
# leave no artifact, so a wrong type or a construct with no lowering can never
# reach the next stage as plausible C.
FAILING_CASES = (
    "backend_unknown_type.l1",
    "backend_alloca_element_type.l1",
    # `#fadd` is recognised but not lowered by this backend.  A marker in an
    # expression was worse than a failure: the operands survived as the comma
    # expression `(x, y)`, which is valid C and silently evaluates to `y`.  The
    # gate therefore requires the refusal, not the marker.
    "backend_float_expr.l1",
)

_STRING = re.compile(r'"(\\.|[^"\\])*"')
_CHAR = re.compile(r"'(\\.|[^'\\])*'")
_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT = re.compile(r"//[^\n]*")
# A definition that starts at column zero and closes its parameter list on this
# line or the next.  Declarations end in `;` and are not definitions.
_TOP_LEVEL_DEF = re.compile(
    r"^(?:void|int|unsigned|uint\d+_t|int\d+_t|float|double|char|size_t|uintptr_t)\b"
)


def strip_noncode(text: str) -> str:
    text = _BLOCK_COMMENT.sub(" ", text)
    text = _LINE_COMMENT.sub(" ", text)
    text = _STRING.sub('""', text)
    text = _CHAR.sub("''", text)
    return text


def analyse(text: str) -> tuple[int, int]:
    """Return (final brace depth, top-level definitions seen inside a body)."""
    depth = 0
    nested = 0
    pending = False
    for line in strip_noncode(text).splitlines():
        stripped = line.strip()
        if pending and stripped.startswith(")"):
            pending = False
            if depth != 0:
                nested += 1
        elif _TOP_LEVEL_DEF.match(line):
            if "(" in line and ")" not in line:
                pending = True
            elif depth != 0:
                nested += 1
        depth += line.count("{") - line.count("}")
    return depth, nested


def run_backend(fixture: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(SEED), "interpreter", str(BACKEND), "main", str(output), str(fixture)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def generate(fixture: Path, output: Path) -> None:
    result = run_backend(fixture, output)
    if result.returncode:
        raise RuntimeError(
            f"{fixture.name}: backend failed: "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )
    if not output.is_file():
        raise RuntimeError(f"{fixture.name}: backend wrote no output")


def main() -> int:
    if not SEED.is_file() or not BACKEND.is_file():
        print(
            "backend C shape: build the seed and the backend entry first",
            file=sys.stderr,
        )
        return 2
    case_names = tuple(case[0] for case in CASES) + FAILING_CASES
    missing = [f for f in case_names if not (FIXTURES / f).is_file()]
    if missing:
        print("backend C shape: missing fixtures: " + ", ".join(missing), file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="lain-backend-shape-", dir=ROOT / "build") as raw:
        work = Path(raw)
        for name, required, forbidden in CASES:
            output = work / f"{Path(name).stem}.c"
            generate(FIXTURES / name, output)
            text = output.read_text(encoding="utf-8", errors="replace")
            absent = [fragment for fragment in required if fragment not in text]
            if absent:
                print(
                    f"{name}: emitted C is missing {absent}",
                    file=sys.stderr,
                )
                return 1
            present = [fragment for fragment in forbidden if fragment in text]
            if present:
                print(
                    f"{name}: emitted C still contains {present}",
                    file=sys.stderr,
                )
                return 1
            depth, nested = analyse(text)
            if depth != 0:
                print(
                    f"{name}: emitted C is not brace balanced "
                    f"(final depth {depth:+d})",
                    file=sys.stderr,
                )
                return 1
            if nested:
                print(
                    f"{name}: {nested} top-level definition(s) emitted inside "
                    f"another function body",
                    file=sys.stderr,
                )
                return 1
            print(f"PASS {name}: emitted C is balanced and flat")
        for name in FAILING_CASES:
            output = work / f"{Path(name).stem}.c"
            result = run_backend(FIXTURES / name, output)
            if result.returncode == 0:
                print(
                    f"{name}: backend accepted a module it cannot lower",
                    file=sys.stderr,
                )
                return 1
            if output.is_file():
                print(
                    f"{name}: backend failed but still wrote {output.name}",
                    file=sys.stderr,
                )
                return 1
            print(f"PASS {name}: backend refused an unlowerable module")
    print(
        "PASS backend C shape: balanced braces, no nested definitions, "
        "unlowerable modules refused"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
