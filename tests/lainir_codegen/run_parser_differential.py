#!/usr/bin/env python3
"""Compare the C reference front end with the LAIN-IR compiler front end."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
CHECKER = ROOT / "bootstrap" / "zig-out" / "bin" / (
    "lainir-print.exe" if sys.platform == "win32" else "lainir-print"
)
BOOTSTRAP = ROOT / "bootstrap" / "zig-out" / "bin" / (
    "lainir-seed.exe" if sys.platform == "win32" else "lainir-seed"
)
COMPILER = ROOT / "src" / "lainir" / "compiler.l1"


CASES = {
    "minimal": (
        True,
        "#proc main() -> #bits<32> { #return 42 }\n",
    ),
    "parameters-and-call": (
        True,
        "#proc add(i32 %a, i32 %b) -> i32 { #return #add(%a, %b) }\n"
        "#proc main() -> i32 { #return #call add(40, 2) }\n",
    ),
    "locals-and-if": (
        True,
        "#proc main() -> i32 { #let %x: i32 = 42 "
        "#if #eq(%x, 42) { #return %x } else { #return 0 } }\n",
    ),
    "loop": (
        True,
        "#proc main() -> i32 { #loop done { #break done } #return 42 }\n",
    ),
    "memory": (
        True,
        "#proc main() -> #bits<32> { #let %p: #addr = #alloca(#bits<32>) "
        "#let %x: #bits<32> = 42 #store[#bits<32>] %x, %p "
        "#return #load[#bits<32>](%p) }\n",
    ),
    "extern-and-effect-call": (
        True,
        "#extern #proc srand(i32 %seed) -> #unit;\n"
        "#proc main() -> i32 { #call srand(1) #return 42 }\n",
    ),
    "explicit-signed-unsigned-integers": (
        True,
        "#proc main() -> #bits<32> { "
        "#let %negative: #bits<8> = 255 "
        "#let %wide: #bits<32> = 84 "
        "#if #slt(%negative, 0) { "
        "#if #uge(%negative, 255) { "
        "#let %one: #bits<8> = #sdiv(%negative, %negative) "
        "#if #eq(%one, 1) { #return #udiv(%wide, 2) } } } "
        "#return 0 }\n",
    ),
    "integer-width-conversions": (
        True,
        "#proc main() -> #bits<32> { "
        "#let %byte: #bits<8> = 255 "
        "#let %zero: #bits<64> = #zext[#bits<64>](%byte) "
        "#let %sign: #bits<64> = #sext[#bits<64>](%byte) "
        "#let %a: #bits<8> = #trunc[#bits<8>](%zero) "
        "#let %b: #bits<8> = #trunc[#bits<8>](%sign) "
        "#if #eq(%a, %b) { #return 42 } #return 0 }\n",
    ),
    "procedure-address-and-indirect-call": (
        True,
        "#proc add(#bits<32> %a, #bits<32> %b) -> #bits<32> { "
        "#return #add(%a, %b) }\n"
        "#proc main() -> #bits<32> { "
        "#let %target: #addr = #proc_addr(add) "
        "#return #call_indirect["
        "(#bits<32>, #bits<32>) -> #bits<32>"
        "](%target, 40, 2) }\n",
    ),
    "float-physical-signature": (
        True,
        "#proc identity(#float<64> %x) -> #float<64> { #return %x }\n"
        "#proc main() -> #bits<32> { #return 42 }\n",
    ),
    "malformed-arrow": (
        False,
        "#proc main() - i32 { #return 0 }\n",
    ),
    "missing-brace": (
        False,
        "#proc main() -> i32 { #return 0\n",
    ),
    "duplicate-procedure": (
        False,
        "#proc main() -> i32 { #return 0 }\n"
        "#proc main() -> i32 { #return 1 }\n",
    ),
    "unknown-call": (
        False,
        "#proc main() -> i32 { #return #call missing() }\n",
    ),
    "wrong-return-width": (
        False,
        "#proc bad(i64 %x) -> i32 { #return %x }\n"
        "#proc main() -> i32 { #return 0 }\n",
    ),
}


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def main() -> int:
    if not CHECKER.exists() or not BOOTSTRAP.exists():
        print("build bootstrap before running parser differential tests", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="lainir-parser-diff-") as directory:
        work = pathlib.Path(directory)
        for name, (expected, source) in CASES.items():
            source_path = work / f"{name}.l1"
            artifact_path = work / f"{name}.c"
            source_path.write_text(source, encoding="utf-8", newline="\n")

            reference = run([str(CHECKER), str(source_path), "main"])
            implemented = run(
                [
                    str(BOOTSTRAP),
                    str(COMPILER),
                    "lainir_compile",
                    str(artifact_path),
                    str(source_path),
                ]
            )
            reference_accepts = reference.returncode == 0
            implemented_accepts = implemented.returncode == 0

            if reference_accepts != expected or implemented_accepts != expected:
                print(
                    f"{name}: expected accepts={expected}, "
                    f"C={reference_accepts}, LAIN-IR={implemented_accepts}",
                    file=sys.stderr,
                )
                if reference.stderr:
                    print(f"C: {reference.stderr.strip()}", file=sys.stderr)
                if implemented.stderr:
                    print(f"LAIN-IR: {implemented.stderr.strip()}", file=sys.stderr)
                return 1

    print(
        f"LAIN-IR parser differential: {len(CASES)} shared acceptance cases agreed"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
