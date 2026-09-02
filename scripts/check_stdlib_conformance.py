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
    return normalized.strip() + "\n"


def compile_with(compiler: Path, fixture: Path, output: Path) -> None:
    result = run(
        [
            str(SEED),
            "interpreter",
            str(compiler),
            "compiler_compile_library",
            str(output),
            str(API),
            str(fixture),
        ]
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"{fixture.name}: compiler failed: {detail}")


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
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL stdlib conformance: {error}", file=sys.stderr)
        raise SystemExit(1)
