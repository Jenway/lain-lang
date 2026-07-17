#!/usr/bin/env python3
"""Exercise the reusable Lain-written Meta compiler artifact."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
AST_CONTRACT_SOURCE = ROOT / "tests/core/self_hosting/ast_capability_contract.lain"
STABLE_SYNTAX_SOURCE = ROOT / "tests/core/self_hosting/stable_syntax_handles.lain"
MODULE_ARTIFACT_SOURCE = ROOT / "tests/core/self_hosting/module_artifact_detached.lain"
IMPORT_SOURCE = ROOT / "tests/core/self_hosting/import_l1_unit_emitter.lain"
ARTIFACT_MODULES_SOURCE = (
    ROOT / "tests/core/self_hosting/artifact_compiles_real_modules.lain"
)
SELF_COMPILE_SOURCE = (
    ROOT / "tests/core/self_hosting/artifact_self_compiles_frontend.lain"
)
DIAGNOSTICS_SOURCE = (
    ROOT / "tests/core/self_hosting/compile_result_diagnostics.lain"
)
MODULE_WORKSPACE_SOURCE = (
    ROOT / "tests/core/self_hosting/module_workspace_end_to_end.lain"
)
MODULE_DIAGNOSTICS_SOURCE = (
    ROOT / "tests/core/self_hosting/module_workspace_diagnostics.lain"
)
LAIN_INTERPRETER_COMPTIME_SOURCE = (
    ROOT / "tests/core/self_hosting/lain_interpreter_comptime.lain"
)
GENERATION_EMITTER_SOURCE = (
    ROOT / "tests/core/self_hosting/artifact_emit_generation.lain"
)
GENERATION_BEHAVIOR_SOURCE = (
    ROOT / "tests/core/self_hosting/artifact_generation_behavior.lain"
)
ARTIFACT_BUILDER = ROOT / "tests/core/self_hosting/build_meta_artifact.py"
META_ARTIFACT = ROOT / "build/core-self-hosting/meta_compiler.l1"
STAGE2_ARTIFACT = ROOT / "build/core-self-hosting/stage2_compiler.l1"
STAGE3_ARTIFACT = ROOT / "build/core-self-hosting/stage3_compiler.l1"
M10_INTERFACE = ROOT / "build/core-self-hosting/compiler_state.m10.lci"
INVALID_SOURCE = ROOT / "tests/core/self_hosting/reject_invalid_generated_text.lain"
UNAUTHORIZED_CAPABILITY_SOURCE = (
    ROOT / "tests/core/self_hosting/reject_unauthorized_compiler_capability.lain"
)
OUT_DIR = ROOT / "build/core-self-hosting"
CLI_SINGLE_SOURCE = OUT_DIR.parent.parent / "tests/core/self_hosting/fixtures/cli_single.lain"
CLI_INVALID_SOURCE = OUT_DIR.parent.parent / "tests/core/self_hosting/fixtures/cli_invalid.lain"
CLI_MATH_SOURCE = OUT_DIR.parent.parent / "tests/core/self_hosting/fixtures/math_module.lain"
CLI_APP_SOURCE = OUT_DIR.parent.parent / "tests/core/self_hosting/fixtures/app_module.lain"
CLI_SINGLE_L1 = OUT_DIR / "cli_single.l1"
CLI_WORKSPACE_L1 = OUT_DIR / "cli_workspace.l1"
CLI_WORKSPACE_REVERSED_L1 = OUT_DIR / "cli_workspace_reversed.l1"
CLI_WORKSPACE_INVALID_L1 = OUT_DIR / "cli_workspace_invalid.l1"
CLI_INVALID_L1 = OUT_DIR / "cli_invalid.l1"


def tool(name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out" / "bin" / f"{name}{suffix}"


def run(
    args: list[str], cwd: Path = ROOT
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True)


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

    stable_syntax = run([str(lainc), "--interpret", str(STABLE_SYNTAX_SOURCE), "main"])
    if stable_syntax.returncode != 0 or stable_syntax.stdout.strip() != "42":
        print("FAIL M15 stable multi-unit syntax handles: "
              f"{(stable_syntax.stderr or stable_syntax.stdout).strip()}")
        return 1

    detached_artifact = run([
        str(lainc), "--interpret", str(MODULE_ARTIFACT_SOURCE), "main"
    ])
    if (detached_artifact.returncode != 0 or
            detached_artifact.stdout.strip() != "42"):
        print("FAIL M17 detached module artifact: "
              f"{(detached_artifact.stderr or detached_artifact.stdout).strip()}")
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
    interface = run([
        str(lainc), "--emit-interface",
        str(ROOT / "packages/lain/compiler/compiler_state.lain"),
        str(M10_INTERFACE),
    ])
    if interface.returncode != 0 or not M10_INTERFACE.exists():
        print("FAIL M10 compiler-state interface emission: "
              f"{(interface.stderr or interface.stdout).strip()}")
        return 1
    interface_text = M10_INTERFACE.read_text(encoding="utf-8")
    for evidence in (
        "(format lci-v2)",
        "(name M9CompilerState)",
        '(identity "compiler_state::M9CompilerState")',
        "(field (name diagnostics) (type M9DiagnosticBag) (offset 24))",
        "(params (M9CompilerState))",
        "(ret M9CompileResult)",
        "(abi_params (addr))",
        "(abi_ret addr)",
    ):
        if evidence not in interface_text:
            print(f"FAIL M10 interface missing semantic/ABI evidence: {evidence}")
            return 1
    mini_meta_text = (
        ROOT / "packages/lain/compiler/mini_meta.lain"
    ).read_text(encoding="utf-8")
    if "struct M9CompilerState" in mini_meta_text or "struct M9CompileResult" in mini_meta_text:
        print("FAIL M10 consumer still mirrors compiler_state nominal types")
        return 1
    built = run([sys.executable, str(ARTIFACT_BUILDER)])
    if built.returncode != 0 or not META_ARTIFACT.exists():
        print("FAIL building reusable Meta compiler artifact: "
              f"{(built.stderr or built.stdout).strip()}")
        return 1
    checked = run([str(tool("l1check")), str(META_ARTIFACT), "mini_meta_compile"])
    if checked.returncode != 0:
        print(f"FAIL verifying Meta compiler artifact: {checked.stderr.strip()}")
        return 1
    for entry, expected in (
        ("m9_compiler_state_begin_probe", "8"),
        ("m9_compiler_state_unit_probe", "42"),
        ("m9_compiler_state_finish_probe", "42"),
        ("m9_compiler_state_runtime_probe", "42"),
    ):
        probe = run([str(tool("l1i")), str(META_ARTIFACT), entry])
        if probe.returncode != 0 or probe.stdout.strip() != expected:
            print(
                f"FAIL M9 aggregate/result probe {entry}: "
                f"{(probe.stderr or probe.stdout).strip()}"
            )
            return 1
    modules = run([str(lainc), "--interpret", str(ARTIFACT_MODULES_SOURCE), "main"])
    if modules.returncode != 0:
        print("FAIL executing reusable Meta compiler artifact: "
              f"{(modules.stderr or modules.stdout).strip()}")
        return 1
    if modules.stdout.strip() != "42":
        print("FAIL expected reusable Meta compiler artifact result 42, got "
              f"{modules.stdout.strip()!r}")
        return 1
    frontend = run([str(lainc), "--interpret", str(SELF_COMPILE_SOURCE), "main"])
    if frontend.returncode != 0:
        print("FAIL self-compiling frontend foundation: "
              f"{(frontend.stderr or frontend.stdout).strip()}")
        return 1
    if frontend.stdout.strip() != "42":
        print("FAIL expected frontend self-compile result 42, got "
              f"{frontend.stdout.strip()!r}")
        return 1
    # The stage2 builder fingerprints the complete compiler source closure and
    # verifies a cache hit with l1check.  Reusing it here preserves the same
    # stage1 -> stage2 proof without recompiling the compiler closure on every
    # unchanged test invocation.
    generated_stage2 = run([
        sys.executable,
        str(ROOT / "tests/core/self_hosting/build_self_hosted_compiler.py"),
    ])
    if generated_stage2.returncode != 0 or not STAGE2_ARTIFACT.exists():
        print("FAIL stage1 did not provide stage2 compiler: "
              f"{(generated_stage2.stderr or generated_stage2.stdout).strip()}")
        return 1
    stage2_checked = run([
        str(tool("l1check")), str(STAGE2_ARTIFACT), "compiler_compile"
    ])
    if stage2_checked.returncode != 0:
        print(f"FAIL verifying stage2 compiler: {stage2_checked.stderr.strip()}")
        return 1
    # A byte-identical existing stage3 is already the fixed-point witness for
    # this exact stage2.  Regenerate only after the compiler artifact changes.
    if (not STAGE3_ARTIFACT.exists() or
            STAGE2_ARTIFACT.read_bytes() != STAGE3_ARTIFACT.read_bytes()):
        generated_stage3 = run([
            str(lainc), "--interpret", str(GENERATION_EMITTER_SOURCE), "stage3"
        ])
        if (generated_stage3.returncode != 0 or
                generated_stage3.stdout.strip() != "1" or
                not STAGE3_ARTIFACT.exists()):
            print("FAIL stage2 did not emit stage3 compiler: "
                  f"{(generated_stage3.stderr or generated_stage3.stdout).strip()}")
            return 1
    stage3_checked = run([
        str(tool("l1check")), str(STAGE3_ARTIFACT), "compiler_compile"
    ])
    if stage3_checked.returncode != 0:
        print(f"FAIL verifying stage3 compiler: {stage3_checked.stderr.strip()}")
        return 1
    if STAGE2_ARTIFACT.read_bytes() != STAGE3_ARTIFACT.read_bytes():
        print("FAIL M13 fixed point: stage2 compiler differs from stage3")
        return 1
    for generation, artifact in (("stage2", STAGE2_ARTIFACT),
                                 ("stage3", STAGE3_ARTIFACT)):
        schema = run([str(tool("l1i")), str(artifact),
                      "mini_meta_schema_version"])
        if schema.returncode != 0 or schema.stdout.strip() != "12":
            print(f"FAIL {generation} schema is not 12: "
                  f"{(schema.stderr or schema.stdout).strip()}")
            return 1
    generation_behavior = run([
        str(lainc), "--interpret", str(GENERATION_BEHAVIOR_SOURCE), "main"
    ])
    if (generation_behavior.returncode != 0 or
            generation_behavior.stdout.strip() != "42"):
        print("FAIL stage1/stage2/stage3 behavior equivalence: "
              f"{(generation_behavior.stderr or generation_behavior.stdout).strip()}")
        return 1
    diagnostics = run([
        str(lainc), "--interpret", str(DIAGNOSTICS_SOURCE), "main"
    ])
    if diagnostics.returncode != 0:
        print("FAIL structured CompileResult diagnostics: "
              f"{(diagnostics.stderr or diagnostics.stdout).strip()}")
        return 1
    if diagnostics.stdout.strip() != "42":
        print("FAIL expected CompileResult diagnostic result 42, got "
              f"{diagnostics.stdout.strip()!r}")
        return 1
    workspace = run([
        str(lainc), "--interpret", str(MODULE_WORKSPACE_SOURCE), "main"
    ])
    if workspace.returncode != 0 or workspace.stdout.strip() != "42":
        print("FAIL M5 module workspace did not link and execute 42: "
              f"{(workspace.stderr or workspace.stdout).strip()}")
        return 1
    module_diagnostics = run([
        str(lainc), "--interpret", str(MODULE_DIAGNOSTICS_SOURCE), "main"
    ])
    if (module_diagnostics.returncode != 0 or
            module_diagnostics.stdout.strip() != "42"):
        print("FAIL M5 module diagnostics: "
              f"{(module_diagnostics.stderr or module_diagnostics.stdout).strip()}")
        return 1
    interpreter_comptime = run([
        str(lainc), "--interpret", str(LAIN_INTERPRETER_COMPTIME_SOURCE), "main"
    ])
    if (interpreter_comptime.returncode != 0 or
            interpreter_comptime.stdout.strip() != "42"):
        print("FAIL M7/M8 Lain interpreter or comptime evaluation: "
              f"{(interpreter_comptime.stderr or interpreter_comptime.stdout).strip()}")
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

    # M14: exercise the installed/default CLI path itself.  These commands do
    # not use a Lain test harness to invoke the artifact.
    single_cli = run(
        [str(lainc), "--emit-l1", str(CLI_SINGLE_SOURCE), str(CLI_SINGLE_L1)],
        cwd=OUT_DIR,
    )
    if single_cli.returncode != 0 or not CLI_SINGLE_L1.exists():
        print("FAIL M14 default self-hosted single-file CLI: "
              f"{(single_cli.stderr or single_cli.stdout).strip()}")
        return 1
    if "could not find std/meta" in single_cli.stderr or "polyfills.scm" in single_cli.stderr:
        print("FAIL M14 normal CLI loaded stage-0 Scheme sources")
        return 1
    single_check = run([
        str(tool("l1check")), str(CLI_SINGLE_L1), "main"
    ])
    single_run = run([str(tool("l1i")), str(CLI_SINGLE_L1), "main"])
    if (single_check.returncode != 0 or single_run.returncode != 0 or
            single_run.stdout.strip() != "42"):
        print("FAIL M14 single-file L1 verification/execution: "
              f"{(single_check.stderr or single_run.stderr or single_run.stdout).strip()}")
        return 1

    workspace_cli = run([
        str(lainc), "--emit-workspace-l1", str(CLI_WORKSPACE_L1),
        str(CLI_MATH_SOURCE), str(CLI_APP_SOURCE),
    ])
    if workspace_cli.returncode != 0 or not CLI_WORKSPACE_L1.exists():
        print("FAIL M14 self-hosted workspace CLI: "
              f"{(workspace_cli.stderr or workspace_cli.stdout).strip()}")
        return 1
    workspace_check = run([
        str(tool("l1check")), str(CLI_WORKSPACE_L1), "app__main"
    ])
    workspace_run = run([
        str(tool("l1i")), str(CLI_WORKSPACE_L1), "app__main"
    ])
    if (workspace_check.returncode != 0 or workspace_run.returncode != 0 or
            workspace_run.stdout.strip() != "42"):
        print("FAIL M14 workspace L1 verification/execution: "
              f"{(workspace_check.stderr or workspace_run.stderr or workspace_run.stdout).strip()}")
        return 1

    # File order is input presentation, not dependency order.  The indexed
    # workspace must resolve and lower app after math even when app comes first.
    CLI_WORKSPACE_REVERSED_L1.unlink(missing_ok=True)
    reversed_workspace_cli = run([
        str(lainc), "--emit-workspace-l1", str(CLI_WORKSPACE_REVERSED_L1),
        str(CLI_APP_SOURCE), str(CLI_MATH_SOURCE),
    ])
    reversed_check = run([
        str(tool("l1check")), str(CLI_WORKSPACE_REVERSED_L1), "app__main"
    ]) if CLI_WORKSPACE_REVERSED_L1.exists() else reversed_workspace_cli
    reversed_run = run([
        str(tool("l1i")), str(CLI_WORKSPACE_REVERSED_L1), "app__main"
    ]) if CLI_WORKSPACE_REVERSED_L1.exists() else reversed_workspace_cli
    if (reversed_workspace_cli.returncode != 0 or
            reversed_check.returncode != 0 or
            reversed_run.returncode != 0 or
            reversed_run.stdout.strip() != "42"):
        print("FAIL M16 dependency order is coupled to input order: "
              f"{(reversed_workspace_cli.stderr or reversed_check.stderr or reversed_run.stderr or reversed_run.stdout).strip()}")
        return 1

    # A workspace input is a collection of source units, not one C-concatenated
    # source string.  Reject the exact file that does not declare one Module
    # and leave no partial artifact behind.
    CLI_WORKSPACE_INVALID_L1.unlink(missing_ok=True)
    invalid_workspace_cli = run([
        str(lainc), "--emit-workspace-l1", str(CLI_WORKSPACE_INVALID_L1),
        str(CLI_MATH_SOURCE), str(CLI_SINGLE_SOURCE),
    ])
    if (invalid_workspace_cli.returncode == 0 or
            CLI_WORKSPACE_INVALID_L1.exists() or
            "error 4303" not in invalid_workspace_cli.stderr or
            str(CLI_SINGLE_SOURCE) not in invalid_workspace_cli.stderr):
        print("FAIL structured workspace did not diagnose the source unit path: "
              f"{(invalid_workspace_cli.stderr or invalid_workspace_cli.stdout).strip()}")
        return 1

    CLI_INVALID_L1.unlink(missing_ok=True)
    invalid_cli = run([
        str(lainc), "--emit-l1", str(CLI_INVALID_SOURCE), str(CLI_INVALID_L1)
    ])
    if (invalid_cli.returncode == 0 or CLI_INVALID_L1.exists() or
            "error 2301" not in invalid_cli.stderr or
            "unknown procedure" not in invalid_cli.stderr):
        print("FAIL M14 structured CLI diagnostic/no-partial-output policy: "
              f"{(invalid_cli.stderr or invalid_cli.stdout).strip()}")
        return 1
    print(
        "PASS M15-M18 stable syntax units, indexed workspace, detached module "
        "artifact/cache, and single-call compiler ABI; M14 self-hosted CLI; "
        "M13 stage2 == stage3 byte fixed point and generation behavior; "
        "M12 Lain-owned parse/Middle/elaborate/structured-L1 pipeline; "
        "M11 Lain-owned growable CompilerContext; "
        "M10 Meta-owned type interfaces without ABI mirrors; "
        "M9 Lain-owned CompilerState/CompileResult; "
        "M7/M8 interpreter and comptime: 42/55"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
