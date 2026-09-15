#!/usr/bin/env python3
"""Guard operator handling in the bootstrap frontend.

Two silent miscompilations lived here, both producing wrong LAINIR with exit
code 0 and no diagnostic:

* a `while` condition kept only its first `&&` conjunct, so the loop exited on
  the first comparison alone;
* a value expression writer that understood a single operator dropped every
  term of lower precedence, so `v * 10 + d` compiled to `#mul(%v, 10)`.

Both are checked by running a program, not by matching emitted text: the point
is the answer, and text assertions would have to be rewritten whenever the
emitter's formatting changes.  The program below combines both cases, so a
regression in either changes the printed value (37 correct, 40 with both
faults, 33 and 44 with one each).
"""

from __future__ import annotations

import subprocess
import sys
import re
import tempfile
from pathlib import Path

from toolchain import seed_exe


ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / "scripts" / "run_lain_compiler.py"
SEED = seed_exe("lainir-seed")

# A `v * 10 + d` whose lower-precedence term must survive, and a `while` whose
# second conjunct must gate the loop.  f(3, 4) is 34 and w(10, 3) is 3, so the
# entry returns 37.
PROGRAM = """\
let f = std::func(v: i32, d: i32) -> i32 {
    let x: i32 = 0;
    x = v * 10 + d;
    return x;
};

let w = std::func(a: i32, b: i32) -> i32 {
    let i: i32 = 0;
    while i < a && i < b {
        i = i + 1;
    }
    return i;
};

let main = std::func() -> i32 {
    return f(3, 4) + w(10, 3);
};
"""

EXPECTED = "37"

# Emitted-text assertions, secondary to the run above but they localise a
# failure to one of the two cases.
TEXT_CASES = (
    ("scripts/fixtures/formal_while_and_condition.lain", "#break loop0", 2,
     "a `while` condition keeps every `&&` conjunct"),
    ("scripts/fixtures/formal_assignment_precedence.lain", r"#add\(#mul\(%[A-Za-z0-9_]+, 10\), %[A-Za-z0-9_]+\)", 2,
     "a mixed-precedence assignment keeps its lower-precedence term"),
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
        print("operator precedence: compiler or seed is missing", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="lain-operators-") as raw:
        work = Path(raw)
        source = work / "operators.lain"
        artifact = work / "operators.l1"
        source.write_text(PROGRAM, encoding="utf-8", newline="\n")
        compiled = run(
            [sys.executable, str(COMPILER), "--library", "-o", str(artifact), str(source)]
        )
        if compiled.returncode:
            print(compiled.stdout + compiled.stderr, file=sys.stderr)
            return 1
        executed = run([str(SEED), "run", str(artifact), "main"])
        actual = executed.stdout.strip()
        if executed.returncode or actual != EXPECTED:
            print(
                f"operator precedence: expected {EXPECTED}, got "
                f"code={executed.returncode}, stdout={actual!r}; "
                f"37 is correct, 40 means both faults, 33 and 44 mean one each",
                file=sys.stderr,
            )
            return 1
        print(f"PASS mixed precedence and `&&` condition: program returned {actual}")

        for name, needle, expected_count, label in TEXT_CASES:
            fixture = ROOT / name
            if not fixture.is_file():
                print(f"operator precedence: missing {name}", file=sys.stderr)
                return 2
            out = work / f"{fixture.stem}.l1"
            built = run(
                [sys.executable, str(COMPILER), "--library", "-o", str(out), str(fixture)]
            )
            if built.returncode:
                print(built.stdout + built.stderr, file=sys.stderr)
                return 1
            text = out.read_text(encoding="utf-8", errors="replace")
            count = len(re.findall(needle, text)) if "assignment_precedence" in name else text.count(needle)
            if count != expected_count:
                print(
                    f"operator precedence: {label}: expected {expected_count} "
                    f"occurrence(s) of {needle!r}, found {count}",
                    file=sys.stderr,
                )
                return 1
            print(f"PASS {label}")
    print("PASS bootstrap frontend operator precedence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
