#!/usr/bin/env python3
"""src/lainc module mechanism acceptance.

Chain: gen1 (frozen lainc) compiles src/lainc/lainc.lain; gen2 (the
Lain-written compiler) compiles module fixtures.  Verifies:
  - namespace isolation: two modules may share member names (f0_<mod>_<m>)
  - qualified calls resolve to prefixed labels and run
  - module constant members resolve qualified references
  - module consteval members fold in top-level initializers
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "seed" / "zig-out" / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""
SEED = BIN / f"lainir-seed{SUFFIX}"
PRINT = BIN / f"lainir-print{SUFFIX}"
FROZEN = ROOT / "src" / "lainir" / "lainc.l1"
LAINC = ROOT / "src" / "lainc" / "lainc.lain"
FIX = Path(__file__).parent / "fixtures"


def run(
    *arguments: Path | str, timeout: float = 300
) -> subprocess.CompletedProcess[str]:
    """Run one compiler step with a hard bound for pathological fixtures."""
    try:
        return subprocess.run(
            [str(a) for a in arguments],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        return subprocess.CompletedProcess(
            [str(a) for a in arguments],
            124,
            stdout=error.stdout or "",
            stderr=f"timeout after {timeout:g}s",
        )


def main() -> int:
    if not SEED.exists() or not PRINT.exists():
        raise RuntimeError("build seed first: cd seed && zig build")
    with tempfile.TemporaryDirectory(prefix="lainc-module-") as directory:
        tmp = Path(directory)
        gen1 = tmp / "gen1.l1"
        gen2 = tmp / "gen2.l1"
        empty = tmp / "empty.lain"
        empty.write_text("", encoding="utf-8")

        # The module fixtures exercise gen2's module lowering, not the costly
        # bootstrap generation itself.  Reuse the fixed-point artifact when
        # explicitly requested, while retaining the full chain by default.
        cached = ROOT / "build" / "debug-gen2-heap.l1"
        if os.environ.get("LAIN_META_USE_CACHED_GEN2") == "1" and cached.exists():
            gen2.write_bytes(cached.read_bytes())
            checked_cache = run(PRINT, gen2, "compiler_compile")
            if checked_cache.returncode:
                raise RuntimeError(f"cached gen2 l1check: {checked_cache.stderr}")
        else:
            compiled = run(SEED, FROZEN, "compiler_compile", gen1, LAINC)
            if compiled.returncode:
                raise RuntimeError(compiled.stderr or compiled.stdout)
            gen2_run = run(SEED, "interpreter", gen1, "compiler_compile", gen2, LAINC)
            if gen2_run.returncode:
                raise RuntimeError(gen2_run.stderr or gen2_run.stdout)

        def compile_fixture(name: str) -> Path:
            out = tmp / f"{name}.l1"
            limit = 120 if name == "module_consteval_member" else 300
            res = run(
                SEED,
                "interpreter",
                gen2,
                "compiler_compile",
                out,
                FIX / f"{name}.lain",
                timeout=limit,
            )
            if res.returncode:
                raise RuntimeError(f"{name}: {res.stderr or res.stdout}")
            checked = run(PRINT, out, "main")
            if checked.returncode:
                raise RuntimeError(f"{name} l1check: {checked.stderr}")
            return out

        def run_main(product: Path) -> str:
            ran = run(SEED, "run", product, "main")
            if ran.returncode:
                raise RuntimeError(f"run {product.name}: {ran.stderr or ran.stdout}")
            return ran.stdout.strip()

        # Namespace isolation: left.add and right.add coexist, main -> 42.
        dup = compile_fixture("module_duplicate_member_names")
        text = dup.read_text(encoding="utf-8")
        assert "#proc f0_left_add(" in text, "missing f0_left_add"
        assert "#proc f0_right_add(" in text, "missing f0_right_add"
        assert "#call f0_left_add(40, 2)" in text, "missing qualified call"
        assert run_main(dup) == "42", "duplicate members run != 42"
        print("PASS namespace isolation (f0_left_add / f0_right_add)")

        # Qualified call with `: Module` annotation -> 42.
        qc = compile_fixture("function_qualified_call")
        text = qc.read_text(encoding="utf-8")
        assert "#proc f0_math_add(" in text, "missing f0_math_add"
        assert "#call f0_math_add(40, 2)" in text, "missing math.add call"
        assert run_main(qc) == "42", "qualified call run != 42"
        print("PASS qualified call (f0_math_add)")

        # Module constant member: app.answer resolves qualified.
        mc = compile_fixture("module_constant_member")
        assert "#proc f0_app_main(" in mc.read_text(encoding="utf-8")
        assert run_main(mc) == "42", "module constant member run != 42"
        print("PASS module constant member (app.answer -> 42)")

        # Module consteval member folds at top level: math.square(6) -> 36.
        mce = compile_fixture("module_consteval_member")
        assert run_main(mce) == "36", "module consteval member run != 36"
        print("PASS module consteval member (math.square(6) -> 36)")
    print("PASS src/lainc module mechanism")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
