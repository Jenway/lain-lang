#!/usr/bin/env python3
"""Compare C-interpreter results with native code emitted by lainir-c."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
BIN = ROOT / "seed" / "zig-out" / "bin"
SUFFIX = ".exe" if sys.platform == "win32" else ""
INTERPRETER = BIN / f"lainir-seed{SUFFIX}"
BOOTSTRAP = BIN / f"lainir-seed{SUFFIX}"
COMPILER = ROOT / "src" / "lainir" / "compiler.l1"
FIXTURES = pathlib.Path(__file__).parent / "fixtures"


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def find_c_compiler() -> list[str] | None:
    zig = shutil.which("zig")
    if zig:
        return [zig, "cc"]
    for candidate in ("clang", "cc", "gcc"):
        found = shutil.which(candidate)
        if found:
            try:
                probe = subprocess.run(
                    [found, "--version"], capture_output=True, timeout=5
                )
            except (OSError, subprocess.SubprocessError):
                continue
            if probe.returncode == 0:
                return [found]
    return None


def main() -> int:
    c_compiler = find_c_compiler()
    if c_compiler is None:
        print("no C compiler available for execution differential", file=sys.stderr)
        return 2

    cases = [
        FIXTURES / "return_42.l1",
        FIXTURES / "width_i8.l1",
        FIXTURES / "explicit_integer_ops.l1",
        FIXTURES / "integer_conversions.l1",
        FIXTURES / "indirect_42.l1",
    ]
    with tempfile.TemporaryDirectory(prefix="lainir-execution-diff-") as directory:
        work = pathlib.Path(directory)
        for source in cases:
            interpreted = run([str(INTERPRETER), str(source), "main"])
            if interpreted.returncode:
                print(f"interpreter failed for {source.name}", file=sys.stderr)
                print(interpreted.stderr, file=sys.stderr)
                return 1
            try:
                expected = int(interpreted.stdout.strip())
            except ValueError:
                print(
                    f"non-integer interpreter result for {source.name}: "
                    f"{interpreted.stdout!r}",
                    file=sys.stderr,
                )
                return 1

            generated_c = work / f"{source.stem}.c"
            executable = work / f"{source.stem}{SUFFIX}"
            generated = run(
                [
                    str(BOOTSTRAP),
                    str(COMPILER),
                    "lainir_compile",
                    str(generated_c),
                    str(source),
                ]
            )
            if generated.returncode:
                print(f"compiler failed for {source.name}", file=sys.stderr)
                print(generated.stderr, file=sys.stderr)
                return 1
            compiled = run(
                [*c_compiler, str(generated_c), "-o", str(executable)]
            )
            if compiled.returncode:
                print(f"C compilation failed for {source.name}", file=sys.stderr)
                print(compiled.stderr, file=sys.stderr)
                return 1
            native = run([str(executable)])
            if native.returncode != expected:
                print(
                    f"{source.name}: interpreter={expected}, "
                    f"native={native.returncode}",
                    file=sys.stderr,
                )
                return 1

    print(f"LAIN-IR execution differential: {len(cases)} programs agreed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
