#!/usr/bin/env python3
"""Canonical structured LAIN-IR and verifier contract tests."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
CHECK = ROOT / "zig-out" / "bin" / ("l1check.exe" if sys.platform == "win32" else "l1check")

VALID = {
    "return": """#proc main() -> i32 { #return 42 }\n""",
    "add": """#proc main() -> i32 { #return #add(40, 2) }\n""",
    "comparison": """#proc main() -> i32 { #if #eq(40, 40) { #return 42 } #return 0 }\n""",
    "if-else": """#proc main() -> i32 { #if #eq(40, 40) { #return 42 } else { #return 0 } }\n""",
    "m2-module": """#proc add(i32 %arg0, i32 %arg1) -> i32 {
  #return #add(%arg0, %arg1)
}
#proc main() -> i32 {
  #let %x: i32 = #call add(40, 2)
  #if #eq(%x, 42) { #return %x } else { #return 0 }
}\n""",
    "call": """#proc answer() -> i32 { #return 42 }
#proc main() -> i32 { #return #call answer() }\n""",
    "if": """#proc main() -> i32 {
  #if 1 { #return 42 }
  #return 0
}\n""",
    "loop": """#proc main() -> i32 { #loop { #break } #return 42 }\n""",
    "typed-binding": """#proc main() -> i32 {
  #let %answer: i32 = #add(40, 2)
  #return %answer
}\n""",
    "legacy-binding": """#proc main() -> i64 {
  #let answer = 42
  #return %answer
}\n""",
    "legacy-label": """#proc main() -> i32 { block_0: #return 42 }\n""",
}

INVALID = {
    "parse": "#proc main( -> i32 { #return 42 }",
    "unknown-call": "#proc main() -> i32 { #return #call missing() }",
    "break-outside-loop": "#proc main() -> i32 { #break #return 42 }",
    "bad-condition": "#proc main() -> i32 { #if 2 { #return 42 } #return 0 }",
    "duplicate-proc": "#proc main() -> i32 { #return 1 } #proc main() -> i32 { #return 2 }",
    "binding-type-mismatch": """#proc main() -> i32 {
  #let %ptr: addr = 42
  #return 0
}""",
    "return-variable-type-mismatch": """#proc main() -> i32 {
  #let %wide: i64 = 42
  #return %wide
}""",
}


def invoke(path: pathlib.Path, entry: str | None = "main") -> subprocess.CompletedProcess[str]:
    command = [str(CHECK), str(path)]
    if entry is not None:
        command.append(entry)
    return subprocess.run(command, cwd=ROOT, text=True, capture_output=True)


def main() -> int:
    if not CHECK.exists():
        print(f"missing contract tool: {CHECK}", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="lainir-contract-") as tmp:
        temp = pathlib.Path(tmp)
        for name, source in VALID.items():
            first = temp / f"{name}.l1"
            first.write_text(source, encoding="utf-8", newline="\n")
            one = invoke(first)
            if one.returncode:
                print(f"{name}: first canonicalization failed\n{one.stderr}", file=sys.stderr)
                return 1
            if "block_0:" in one.stdout:
                print(f"{name}: canonical output leaked a legacy block label", file=sys.stderr)
                return 1
            if name == "legacy-binding" and "#let %answer: i64 = 42" not in one.stdout:
                print(f"{name}: legacy binding was not canonicalized as typed", file=sys.stderr)
                return 1
            second = temp / f"{name}.canonical.l1"
            second.write_text(one.stdout, encoding="utf-8", newline="\n")
            two = invoke(second)
            if two.returncode or one.stdout != two.stdout:
                print(f"{name}: parse/emit round trip is unstable\n{two.stderr}", file=sys.stderr)
                return 1
        for name, source in INVALID.items():
            path = temp / f"invalid-{name}.l1"
            path.write_text(source, encoding="utf-8", newline="\n")
            result = invoke(path)
            if result.returncode == 0:
                print(f"{name}: invalid module was accepted", file=sys.stderr)
                return 1
            if "parse[" not in result.stderr and "verify[" not in result.stderr:
                print(f"{name}: failure was not structured: {result.stderr}", file=sys.stderr)
                return 1
    print(f"LAIN-IR contract: {len(VALID)} round trips, {len(INVALID)} diagnostics passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
