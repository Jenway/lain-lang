#!/usr/bin/env python3
"""Audit bootstrap library ownership and capture each generated #eval artifact.

The capture probe changes a temporary compiler bundle, never source or cached
build artifacts. It intercepts one selected wrapper, writes its pending IR to
the probe output, and deliberately traps before executing it. The original
bundle must still compile and run the fixture. This proves path reachability
and actual IR generation, not complete source-language effect semantics.
"""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from build_lain_compiler import BOOTSTRAP_STD_MODULES, CORE_MODULES
from toolchain import seed_exe

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "scripts" / "fixtures"
SEED = seed_exe("lainir-seed")
PRINT = seed_exe("lainir-print")
CASES = (
    ("arithmetic", "formal_consteval_arithmetic.lain", "lainvm_eval_consteval_group", "meta_eval"),
    ("scalar", "formal_meta_scalar_call.lain", "lainvm_eval_meta_scalar_call", "meta_entry"),
    ("module", "formal_meta_module_factory.lain", "lainvm_eval_meta_module_call", "meta_entry"),
    ("effect", "formal_meta_effect_factory.lain", "lainvm_eval_meta_effect_call", "meta_entry"),
    ("operation", "formal_meta_effect_factory.lain", "lainvm_eval_meta_effect_operation", "meta_entry"),
)
PROBE = """
#extern #proc bootstrap.write-artifact(#addr %data, #bits<64> %length) -> #unit;
#proc audit_capture_artifact(#addr %arguments, #bits<64> %physical_kind) -> #bits<64> {
  #call lainvm_capture_finish()
  #call bootstrap.write-artifact(#call lainvm_capture_data(), #call lainvm_capture_length())
  #let %audit_trap: #bits<64> = #sdiv(1, 0)
  #return %audit_trap
}
"""


def run(*args: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(x) for x in args], cwd=ROOT, capture_output=True, text=True)


def require(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode:
        raise RuntimeError(f"{label}: {result.stderr.strip() or result.stdout.strip()}")


def proc_region(text: str, name: str) -> tuple[int, int]:
    """Bound a top-level procedure by the next declaration, not brace counting.

    Generated IR strings contain braces, so raw brace counting is unsuitable.
    """
    match = re.search(r"^#proc " + re.escape(name) + r"\(", text, re.M)
    if not match:
        raise RuntimeError(f"missing procedure {name}")
    end = re.search(r"^#(?:proc|extern|data)\b", text[match.end():], re.M)
    return match.start(), match.end() + end.start() if end else len(text)


def instrument(text: str, wrapper: str) -> str:
    start, end = proc_region(text, wrapper)
    body = text[start:end]
    if wrapper == "lainvm_eval_consteval_group":
        old = "  #call lainvm_capture_finish()"
        new = old + "\n  #call bootstrap.write-artifact(#call lainvm_capture_data(), #call lainvm_capture_length())\n  #let %audit_trap: #bits<64> = #sdiv(1, 0)"
    else:
        old, new = "#call lainvm_meta_run_artifact(", "#call audit_capture_artifact("
    if body.count(old) != 1:
        raise RuntimeError(f"{wrapper}: expected exactly one intercept point")
    return text[:start] + body.replace(old, new) + text[end:] + PROBE


def main() -> int:
    require(run(sys.executable, ROOT / "scripts" / "build_lain_compiler.py"), "build")
    bundle_path = ROOT / "build" / "bootstrap" / "lainc.l1"
    bundle = bundle_path.read_text(encoding="utf-8")
    core = (bundle_path.parent / "compiler_core.l1").read_text(encoding="utf-8")
    library = (bundle_path.parent / "stdlib.l1").read_text(encoding="utf-8")
    report: dict[str, object] = {
        "bundle_sha256": hashlib.sha256(bundle_path.read_bytes()).hexdigest(),
        "core_sources": [p.relative_to(ROOT).as_posix() for p in CORE_MODULES],
        "library_sources": [p.relative_to(ROOT).as_posix() for p in BOOTSTRAP_STD_MODULES],
        "paths": [],
        "limitations": [
            "effect fixture main returns 42 independently of effect/operation values",
            "capture traps before executing intercepted IR; baseline executes the original bundle",
            "does not prove complete body lowering, staged elaboration, shared VSpace, or effect handler semantics",
        ],
    }
    output = ROOT / "build" / "meta-pipeline-audit"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="meta-audit-", dir=ROOT / "build") as raw:
        work = Path(raw)
        for label, fixture_name, wrapper, entry in CASES:
            if re.search(r"^#proc " + re.escape(wrapper) + r"\(", core, re.M):
                raise RuntimeError(f"{wrapper}: semantic wrapper leaked into core")
            proc_region(library, wrapper)
            owners = []
            for source in BOOTSTRAP_STD_MODULES:
                source_text = source.read_text(encoding="utf-8")
                if re.search(r"^#proc " + re.escape(wrapper) + r"\(", source_text, re.M):
                    owners.append(source.relative_to(ROOT).as_posix())
            if len(owners) != 1:
                raise RuntimeError(f"{wrapper}: expected one library source owner")
            fixture = FIXTURES / fixture_name
            artifact = work / f"{label}-runtime.l1"
            require(run(SEED, "interpreter", bundle_path, "compiler_compile_library", artifact,
                        fixture, FIXTURES / "empty_source.lain"), f"{label} baseline compile")
            require(run(PRINT, artifact, "main"), f"{label} baseline verify")
            result = run(SEED, "run", artifact, "main")
            require(result, f"{label} baseline execute")
            if result.stdout.strip() != "42":
                raise RuntimeError(f"{label}: expected 42, got {result.stdout.strip()!r}")
            temporary_bundle = work / f"{label}-probe.l1"
            temporary_bundle.write_text(instrument(bundle, wrapper), encoding="utf-8", newline="\n")
            require(run(PRINT, temporary_bundle, "compiler_compile_library"), f"{label} probe verify")
            captured = work / f"{label}-captured.l1"
            intercepted = run(SEED, "interpreter", temporary_bundle, "compiler_compile_library",
                              captured, fixture, FIXTURES / "empty_source.lain")
            if not intercepted.returncode or "sdiv by zero" not in intercepted.stderr:
                raise RuntimeError(f"{label}: capture intercept was not reached: {intercepted.stderr}")
            ir = captured.read_text(encoding="utf-8")
            if not re.search(r"#(?:let\s+%value:\s+#bits<64>\s*=|return)\s+#eval\s*\{", ir):
                raise RuntimeError(f"{label}: generated artifact has no explicit #eval")
            require(run(PRINT, captured, entry), f"{label} captured IR verify")
            destination = output / f"{label}-generated.l1"
            destination.write_text(ir, encoding="utf-8", newline="\n")
            report["paths"].append({
                "path": label, "fixture": fixture.relative_to(ROOT).as_posix(),
                "wrapper": wrapper, "owner": owners[0], "artifact_owner": "stdlib.l1",
                "baseline_result": 42, "capture_probe": "verified bundle then runtime sdiv trap",
                "generated_ir": destination.relative_to(ROOT).as_posix(),
                "generated_ir_sha256": hashlib.sha256(ir.encode("utf-8")).hexdigest(),
                "contains_eval": True,
                "generated_externs": re.findall(r"^#extern #proc ([^\s(]+)", ir, re.M),
                "wrapper_calls": sorted(set(re.findall(
                    r"#call ([A-Za-z_][A-Za-z0-9_.-]*)",
                    re.sub(r'"(?:\\.|[^"\\])*"', '',
                           bundle[slice(*proc_region(bundle, wrapper))]),
                ))),
            })
            print(f"PASS {label}: library-owned wrapper reached, generated #eval verified, baseline 42", flush=True)
    (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                                        encoding="utf-8", newline="\n")
    print("PASS bootstrap Meta pipeline audit (ownership and generation; limitations in report)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError) as error:
        print(f"FAIL Meta pipeline audit: {error}", file=sys.stderr)
        raise SystemExit(1)
