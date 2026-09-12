#!/usr/bin/env python3
"""Guard the `std::func` signature shape: input row, effect row, and arrow."""

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
FULL = ROOT / "scripts" / "fixtures" / "formal_signature_full.lain"
EMPTY_SOURCE = ROOT / "scripts" / "fixtures" / "empty_source.lain"

# Signatures that must be accepted: rows may be omitted, empty, or carry
# several `name : type` entries.
POSITIVE = (
    ("full rows", None),  # the checked-in formal_signature_full.lain fixture
    (
        "empty rows",
        "let f = std::func(value: i64) ?{} -> i64 !{} {\n"
        "    return value;\n"
        "};\n"
        "\n"
        "let main = std::func() -> i64 {\n"
        "    return f(42);\n"
        "};\n",
    ),
    (
        "omitted rows",
        "let f = std::func(value: i64) -> i64 {\n"
        "    return value;\n"
        "};\n"
        "\n"
        "let main = std::func() -> i64 {\n"
        "    return f(42);\n"
        "};\n",
    ),
    (
        "multi-entry input row",
        "let f = std::func(value: i64) ?{T: i64, U: i64} -> i64 {\n"
        "    return value;\n"
        "};\n"
        "\n"
        "let main = std::func() -> i64 {\n"
        "    return f(42);\n"
        "};\n",
    ),
)

# Signatures that must be rejected with the listed bootstrap status code.
NEGATIVE = (
    (
        "duplicate input row",
        "let f = std::func(value: i64) ?{T: i64} ?{U: i64} -> i64 {\n"
        "    return value;\n"
        "};\n",
        5102,
    ),
    (
        "duplicate effect row",
        "let f = std::func(value: i64) -> i64 !{IO} !{IO} {\n"
        "    return value;\n"
        "};\n",
        5102,
    ),
    (
        "effect row before the arrow",
        "let f = std::func(value: i64) !{IO} -> i64 {\n"
        "    return value;\n"
        "};\n",
        5102,
    ),
    (
        "missing arrow",
        "let f = std::func(v: i64) i64 {\n"
        "    return v;\n"
        "};\n",
        5102,
    ),
    (
        "missing body",
        "let f = std::func(v: i64) -> i64\n",
        5102,
    ),
    (
        "input entry missing a name",
        "let f = std::func(value: i64) ?{: i64} -> i64 {\n"
        "    return value;\n"
        "};\n",
        5102,
    ),
    (
        "input entry missing a type",
        "let f = std::func(value: i64) ?{T:} -> i64 {\n"
        "    return value;\n"
        "};\n",
        5102,
    ),
    (
        "input entries missing a separator",
        "let f = std::func(value: i64) ?{T: i64 U: i64} -> i64 {\n"
        "    return value;\n"
        "};\n",
        5102,
    ),
)

# For these rejections the unit validation span must point at the token that
# makes the input row ill-formed, not at the whole declaration.
SPANS = (
    ("input entry missing a name", ":"),
    ("input entry missing a type", ":"),
    ("input entries missing a separator", "U"),
)

NODE_PREFIX = "(context node "


def run(arguments: list[Path | str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def context_node(artifact: Path) -> str:
    if not artifact.is_file():
        return ""
    for line in artifact.read_text(encoding="utf-8").splitlines():
        if line.startswith(NODE_PREFIX):
            return line
    return ""


def check_shape(directory: Path) -> bool:
    dump = directory / "signature.dump"
    dumped = run(
        [SEED, "interpreter", BUNDLE, "lain_raw_ast_dump", dump, FULL, EMPTY_SOURCE]
    )
    if dumped.returncode or not dump.is_file():
        print(dumped.stderr or dumped.stdout, file=sys.stderr)
        return False
    text = dump.read_text(encoding="utf-8")
    assertions = (
        ("one `?` marker", text.count("(atom ?)") == 1),
        ("one `!` marker", text.count("(atom !)") == 1),
        ("the arrow is two adjacent atoms", "(atom -) (atom >)" in text),
        ("the arrow is not one atom", "(atom ->)" not in text),
        (
            "the input row is one `{...}` group",
            "(atom ?) (group { (atom T) (atom :) (atom i64))" in text,
        ),
        (
            "the effect row is one `{...}` group",
            "(atom !) (group { (atom IO))" in text,
        ),
    )
    for label, passed in assertions:
        if not passed:
            print(f"RawAst shape assertion failed: {label}", file=sys.stderr)
            print(text, file=sys.stderr)
            return False
    return True


def main() -> int:
    if not BUNDLE.is_file():
        print(f"missing bootstrap compiler bundle: {BUNDLE}", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="lain-function-signature-") as temp:
        directory = Path(temp)
        sizes: list[str] = []
        for index, (label, source) in enumerate(POSITIVE):
            if source is None:
                fixture = FULL
            else:
                fixture = directory / f"positive_{index}.lain"
                fixture.write_text(source, encoding="utf-8")
            artifact = directory / f"positive_{index}.l1"
            compiled = run(
                [sys.executable, COMPILER, "--library", "-o", artifact, fixture]
            )
            if compiled.returncode or not artifact.is_file() or not artifact.stat().st_size:
                print(compiled.stderr or compiled.stdout, file=sys.stderr)
                print(f"valid signature rejected: {label}", file=sys.stderr)
                return 1
            sizes.append(f"{label}={artifact.stat().st_size}B")
        if not check_shape(directory):
            return 1
        sources = {}
        for index, (label, source, code) in enumerate(NEGATIVE):
            sources[label] = source
            fixture = directory / f"negative_{index}.lain"
            fixture.write_text(source, encoding="utf-8")
            artifact = directory / f"negative_{index}.l1"
            compiled = run(
                [sys.executable, COMPILER, "--library", "-o", artifact, fixture]
            )
            if compiled.returncode == 0:
                print(f"invalid signature accepted: {label}", file=sys.stderr)
                return 1
            message = (compiled.stderr or "") + (compiled.stdout or "")
            if f"status {code}" not in message:
                print(
                    f"{label}: expected bootstrap status {code}, got {message.strip()}",
                    file=sys.stderr,
                )
                return 1
        for label, token in SPANS:
            source = sources[label]
            fixture = directory / "span.lain"
            fixture.write_text(source, encoding="utf-8")
            artifact = directory / "span.l1"
            run(
                [
                    SEED,
                    BUNDLE,
                    "compiler_compile_library",
                    artifact,
                    fixture,
                    EMPTY_SOURCE,
                ]
            )
            line = context_node(artifact)
            if not line:
                print(f"{label}: no validation node span in the error artifact", file=sys.stderr)
                return 1
            node = line[len(NODE_PREFIX) :].split(" start=")[0]
            start = int(line.split("start=")[1].split(" ")[0])
            if node != token or source[start] != token:
                print(
                    f"{label}: validation span {node!r} at {start} is not {token!r}",
                    file=sys.stderr,
                )
                return 1
    print(
        "PASS function signature rows, arrow shape, and input entry validation "
        f"({', '.join(sizes)})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
