#!/usr/bin/env python3
"""Guard `#bits<N>` physical width resolution.

`#bits<N>` is not one source Atom: the lexer splits it into four siblings
(`#bits`, `<`, digits, `>`), so a width resolver cannot compare a single node
against the whole spelling.  This check pins the behaviour end to end in
program mode: the declared width reaches the emitted LAIN-IR signature and the
running program, each declared width is honoured (`#bits<8>` narrows a 300
argument to 44 while `#bits<32>` keeps it), and a width that is not a whole
number of bytes stays unknown.

Before the support existed these parameter annotations were rejected as
unresolvable types (5104), so the check has discriminating power: it fails on a
compiler that only recognises the named builtins.
"""

from __future__ import annotations

import subprocess
import re
import sys
import tempfile
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / "scripts" / "run_lain_compiler.py"
FIXTURES = ROOT / "scripts" / "fixtures"
SEED = seed_exe("lainir-seed")

# (label, fixture, emitted parameter spelling, expected stdout).  The three
# fixtures are the same program shape; only the declared width differs.
RUNNABLE = (
    ("8-bit parameter", "formal_bits_param_8.lain", "(#bits<8> %x)", "44"),
    ("32-bit parameter", "formal_bits_param_32.lain", "(#bits<32> %x)", "300"),
    ("64-bit parameter", "formal_bits_param_64.lain", "(#bits<64> %x)", "300"),
)

# (label, source, status) -- `#bits<N>` spellings that must stay unknown: `12`
# is not a whole number of bytes, and the second one never closes the width.
REFUSED = (
    (
        "width is not a byte size",
        "let odd = std::func(x: #bits<12>) -> u32 {\n    return 1;\n};\n\n"
        "let main = std::func() -> u32 {\n    return odd(1);\n};\n",
        5104,
    ),
    (
        "unterminated width",
        "let main = std::func(x: #bits<32) -> u32 {\n    return x;\n};\n",
        5104,
    ),
)


def run(arguments: list[Path | str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def main() -> int:
    if not COMPILER.is_file() or not SEED.is_file():
        print(
            "bits type: run_lain_compiler.py or the seed is missing",
            file=sys.stderr,
        )
        return 2
    with tempfile.TemporaryDirectory(prefix="lain-bits-type-") as raw:
        work = Path(raw)
        for label, name, signature, expected in RUNNABLE:
            artifact = work / f"{name}.l1"
            compiled = run(
                [
                    sys.executable,
                    str(COMPILER),
                    "-o",
                    str(artifact),
                    str(FIXTURES / name),
                ]
            )
            if compiled.returncode:
                print(
                    f"{label}: a `#bits<N>` annotation was refused: "
                    f"{compiled.stdout + compiled.stderr}".strip(),
                    file=sys.stderr,
                )
                return 1
            text = artifact.read_text(encoding="utf-8")
            physical_signature = re.escape(signature).replace(re.escape("%x"), r"%[A-Za-z_][A-Za-z0-9_]*")
            if re.search(physical_signature, text) is None:
                print(
                    f"{label}: emitted signature is missing {signature!r}: "
                    f"{text.strip()}",
                    file=sys.stderr,
                )
                return 1
            executed = run([str(SEED), "run", str(artifact), "main"])
            actual = executed.stdout.strip()
            if executed.returncode or actual != expected:
                print(
                    f"{label}: expected {expected!r}, got "
                    f"code={executed.returncode}, stdout={actual!r}",
                    file=sys.stderr,
                )
                return 1
            print(f"PASS {label}: declared physical width emitted, ran to {actual}")

        for index, (label, source, status) in enumerate(REFUSED):
            fixture = work / f"refused_{index}.lain"
            artifact = work / f"refused_{index}.l1"
            fixture.write_text(source, encoding="utf-8", newline="\n")
            compiled = run(
                [
                    sys.executable,
                    str(COMPILER),
                    "-o",
                    str(artifact),
                    str(fixture),
                ]
            )
            detail = compiled.stdout + compiled.stderr
            if compiled.returncode == 0:
                print(
                    f"{label}: an unusable `#bits<N>` width was accepted",
                    file=sys.stderr,
                )
                return 1
            if str(status) not in detail:
                print(
                    f"{label}: expected status {status}, got: {detail.strip()}",
                    file=sys.stderr,
                )
                return 1
            print(f"PASS {label}: refused with {status}")
    print("PASS #bits<N> physical width resolution")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
