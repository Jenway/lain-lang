#!/usr/bin/env python3
"""Exercise the coarse-grained generated-L1 execution capability."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "tests/core/self_hosting/execute_generated_text.lain"
AST_CONTRACT_SOURCE = ROOT / "tests/core/self_hosting/ast_capability_contract.lain"
IMPORT_SOURCE = ROOT / "tests/core/self_hosting/import_l1_unit_emitter.lain"
M3_ARTIFACT_SOURCE = ROOT / "tests/core/self_hosting/m3_artifact_end_to_end.lain"
M4_FRONTEND_SOURCE = ROOT / "tests/core/self_hosting/m4_self_compile_frontend.lain"
M4_DIAGNOSTICS_SOURCE = (
    ROOT / "tests/core/self_hosting/m4_compile_result_diagnostics.lain"
)
M3_ARTIFACT_BUILDER = ROOT / "tests/core/self_hosting/build_m3_artifact.py"
M3_ARTIFACT = ROOT / "build/core-self-hosting/m3_meta_compiler.l1"
INVALID_SOURCE = ROOT / "tests/core/self_hosting/reject_invalid_generated_text.lain"
UNAUTHORIZED_CAPABILITY_SOURCE = (
    ROOT / "tests/core/self_hosting/reject_unauthorized_compiler_capability.lain"
)
OUT_DIR = ROOT / "build/core-self-hosting"
OUT_L1 = OUT_DIR / "execute_generated_text.l1"


def tool(name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out" / "bin" / f"{name}{suffix}"


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    lainc = tool("lainc")
    if not lainc.exists():
        print("FAIL self-hosting tools missing; run zig build first")
        return 1

    contract = run([str(lainc), "--interpret", str(AST_CONTRACT_SOURCE), "main"])
    if contract.returncode != 0:
        print("FAIL executing ast.* capability contract: "
              f"{(contract.stderr or contract.stdout).strip()}")
        return 1
    if contract.stdout.strip() != "42":
        print(f"FAIL ast.* capability contract returned {contract.stdout.strip()!r}")
        return 1

    # --interpret compiles the Lain component and executes it through the
    # in-process interpreter, preserving its explicit host capability table.
    executed = run([str(lainc), "--interpret", str(SOURCE), "main"])
    if executed.returncode != 0:
        print(f"FAIL executing stage-1 component: {(executed.stderr or executed.stdout).strip()}")
        return 1
    if executed.stdout.strip() != "42":
        print(f"FAIL expected generated target result 42, got {executed.stdout.strip()!r}")
        return 1
    imported = run([str(lainc), "--interpret", str(IMPORT_SOURCE), "main"])
    if imported.returncode != 0:
        print("FAIL executing imported L1Unit emitter: "
              f"{(imported.stderr or imported.stdout).strip()}")
        return 1
    if imported.stdout.strip() != "42":
        print("FAIL expected imported L1Unit emitter result 42, got "
              f"{imported.stdout.strip()!r}")
        return 1
    built = run([sys.executable, str(M3_ARTIFACT_BUILDER)])
    if built.returncode != 0 or not M3_ARTIFACT.exists():
        print("FAIL building reusable M3 compiler artifact: "
              f"{(built.stderr or built.stdout).strip()}")
        return 1
    checked = run([str(tool("l1check")), str(M3_ARTIFACT), "mini_meta_compile"])
    if checked.returncode != 0:
        print(f"FAIL verifying M3 compiler artifact: {checked.stderr.strip()}")
        return 1
    m3 = run([str(lainc), "--interpret", str(M3_ARTIFACT_SOURCE), "main"])
    if m3.returncode != 0:
        print("FAIL executing reusable M3 compiler artifact: "
              f"{(m3.stderr or m3.stdout).strip()}")
        return 1
    if m3.stdout.strip() != "42":
        print(f"FAIL expected reusable M3 compiler artifact result 42, got {m3.stdout.strip()!r}")
        return 1
    frontend = run([str(lainc), "--interpret", str(M4_FRONTEND_SOURCE), "main"])
    if frontend.returncode != 0:
        print("FAIL M4 self-compiling frontend foundation: "
              f"{(frontend.stderr or frontend.stdout).strip()}")
        return 1
    if frontend.stdout.strip() != "42":
        print("FAIL expected M4 frontend self-compile result 42, got "
              f"{frontend.stdout.strip()!r}")
        return 1
    diagnostics = run([
        str(lainc), "--interpret", str(M4_DIAGNOSTICS_SOURCE), "main"
    ])
    if diagnostics.returncode != 0:
        print("FAIL M4 structured CompileResult diagnostics: "
              f"{(diagnostics.stderr or diagnostics.stdout).strip()}")
        return 1
    if diagnostics.stdout.strip() != "42":
        print("FAIL expected M4 CompileResult diagnostic result 42, got "
              f"{diagnostics.stdout.strip()!r}")
        return 1
    rejected = run([str(lainc), "--interpret", str(INVALID_SOURCE), "main"])
    if rejected.returncode == 0:
        print("FAIL invalid generated L1 text was executed")
        return 1
    unauthorized = run([
        str(lainc), "--interpret", str(UNAUTHORIZED_CAPABILITY_SOURCE), "main"
    ])
    # The embedded Scheme host currently represents an FFI exception as a VM
    # value, so the outer test program may still exit normally.  The security
    # contract is the pre-execution policy diagnostic: the forbidden extern
    # must never be bound or invoked.
    if "unauthorized capability" not in (unauthorized.stderr + unauthorized.stdout):
        print("FAIL unauthorized compiler capability did not produce policy diagnostic: "
              f"{(unauthorized.stderr or unauthorized.stdout).strip()}")
        return 1
    print("PASS M4 artifact self-compiled the complete mini frontend core and diagnostics: 42")
    return 0


if __name__ == "__main__":
    sys.exit(main())
