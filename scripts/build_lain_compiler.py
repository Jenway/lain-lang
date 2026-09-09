#!/usr/bin/env python3
"""Build the first executable Lain-to-LAIN-IR compiler slice."""

from __future__ import annotations

import os
import hashlib
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
BOUNDARY_CHECK = ROOT / "scripts" / "check_lainir_boundaries.py"
L1CHECK = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-print.exe" if os.name == "nt" else "lainir-print"
)
OUTPUT = ROOT / "build" / "bootstrap" / "lainc.l1"
CORE_OUTPUT = ROOT / "build" / "bootstrap" / "compiler_core.l1"
BOOTSTRAP_STD_OUTPUT = ROOT / "build" / "bootstrap" / "stdlib.l1"
STAMP = OUTPUT.with_suffix(".stamp.json")
CORE_STAMP = CORE_OUTPUT.with_suffix(".stamp.json")
BOOTSTRAP_STD_STAMP = BOOTSTRAP_STD_OUTPUT.with_suffix(".stamp.json")
BOOTSTRAP_STD_MANIFEST = BOOTSTRAP_STD_OUTPUT.with_suffix(".manifest.json")
SCHEMA = "lain-compiler-bundle-v1"
CORE_MODULES = (
    ROOT / "bootstrap" / "compiler" / "source.l1",
    ROOT / "bootstrap" / "compiler" / "raw_ast.l1",
    ROOT / "bootstrap" / "compiler" / "ast_runtime.l1",
    ROOT / "bootstrap" / "compiler" / "compiler_context.l1",
    ROOT / "bootstrap" / "compiler" / "stdlib_contracts.l1",
    ROOT / "bootstrap" / "compiler" / "compiler_api.l1",
    ROOT / "bootstrap" / "compiler" / "compiler.l1",
)
BOOTSTRAP_STD_MODULES = (
    # Semantic Meta and lowering implementations belong to the bootstrap
    # standard library.  The compiler core sees only their ABI declarations.
    ROOT / "bootstrap" / "compiler" / "meta.l1",
    ROOT / "bootstrap" / "compiler" / "meta_values.l1",
    ROOT / "bootstrap" / "compiler" / "meta_eval_vm.l1",
    ROOT / "bootstrap" / "compiler" / "lower_func.l1",
    ROOT / "bootstrap" / "compiler" / "lower_record.l1",
    ROOT / "bootstrap" / "compiler" / "lower_program.l1",
    ROOT / "bootstrap" / "compiler" / "meta_bindings.l1",
    # Import discovery and syntax-index construction are language policy;
    # keep them with the bootstrap library rather than the core driver.
    ROOT / "bootstrap" / "compiler" / "workspace.l1",
    ROOT / "bootstrap" / "compiler" / "workspace_cache.l1",
    ROOT / "bootstrap" / "compiler" / "syntax_units.l1",
    ROOT / "bootstrap" / "compiler" / "meta_import.l1",
    ROOT / "bootstrap" / "compiler" / "meta_record.l1",
    ROOT / "bootstrap" / "compiler" / "meta_type.l1",
    ROOT / "bootstrap" / "compiler" / "meta_call.l1",
    ROOT / "bootstrap" / "compiler" / "meta_collect.l1",
    ROOT / "bootstrap" / "std" / "policy.l1",
    ROOT / "bootstrap" / "compiler" / "module_meta.l1",
    ROOT / "bootstrap" / "compiler" / "meta_module.l1",
    ROOT / "bootstrap" / "std" / "core_forms.l1",
    ROOT / "bootstrap" / "std" / "entry.l1",
)
MODULES = CORE_MODULES + BOOTSTRAP_STD_MODULES


def run(arguments: list[Path | str]) -> None:
    result = subprocess.run(
        [str(argument) for argument in arguments],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def fingerprint_for(inputs: tuple[Path, ...]) -> str:
    digest = hashlib.sha256(SCHEMA.encode("utf-8"))
    digest.update(BUNDLER.read_bytes())
    for path in inputs:
        digest.update(str(path.relative_to(ROOT)).replace("\\", "/").encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def input_fingerprint() -> str:
    return fingerprint_for(MODULES)


def write_bundle(output: Path, inputs: tuple[Path, ...]) -> None:
    run([sys.executable, BUNDLER, "-o", output, *inputs])


def write_stamp_for(output: Path, stamp: Path, fingerprint: str) -> None:
    stamp.write_text(
        json.dumps(
            {
                "schema": SCHEMA,
                "inputs": fingerprint,
                "output": hashlib.sha256(output.read_bytes()).hexdigest(),
                "artifact": str(output.relative_to(ROOT)).replace("\\", "/"),
            },
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )


def cache_valid(fingerprint: str) -> bool:
    if not OUTPUT.is_file() or not STAMP.is_file():
        return False
    try:
        stamp = json.loads(STAMP.read_text(encoding="utf-8"))
        output_hash = hashlib.sha256(OUTPUT.read_bytes()).hexdigest()
    except (OSError, ValueError, TypeError):
        return False
    return (
        stamp.get("schema") == SCHEMA
        and stamp.get("inputs") == fingerprint
        and stamp.get("output") == output_hash
    )


def write_stamp(fingerprint: str) -> None:
    write_stamp_for(OUTPUT, STAMP, fingerprint)


def write_bootstrap_std_manifest(fingerprint: str) -> None:
    sources = []
    for path in BOOTSTRAP_STD_MODULES:
        sources.append(
            {
                "path": str(path.relative_to(ROOT)).replace("\\", "/"),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
        )
    BOOTSTRAP_STD_MANIFEST.write_text(
        json.dumps(
            {
                "schema": "lain-bootstrap-stdlib-v1",
                "abi": "lain_std_abi_v1",
                "abi_version": 1,
                "artifact": str(BOOTSTRAP_STD_OUTPUT.relative_to(ROOT)).replace("\\", "/"),
                "artifact_sha256": hashlib.sha256(BOOTSTRAP_STD_OUTPUT.read_bytes()).hexdigest(),
                "inputs_sha256": fingerprint,
                "target_independent": True,
                "sources": sources,
            },
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )


def main() -> int:
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    fingerprint = input_fingerprint()
    if cache_valid(fingerprint) and CORE_OUTPUT.is_file() and BOOTSTRAP_STD_OUTPUT.is_file():
        try:
            run([L1CHECK, OUTPUT, "compiler_compile"])
            run([L1CHECK, CORE_OUTPUT, "compiler_compile"])
            run([L1CHECK, BOOTSTRAP_STD_OUTPUT, "lain_std_abi_version"])
            run([sys.executable, BOUNDARY_CHECK])
        except RuntimeError:
            pass
        else:
            write_bootstrap_std_manifest(fingerprint_for(BOOTSTRAP_STD_MODULES))
            print(
                f"{OUTPUT.relative_to(ROOT)} + {CORE_OUTPUT.relative_to(ROOT)} + "
                f"{BOOTSTRAP_STD_OUTPUT.relative_to(ROOT)} (cache hit)"
            )
            return 0
    core_fingerprint = fingerprint_for(CORE_MODULES)
    std_fingerprint = fingerprint_for(BOOTSTRAP_STD_MODULES)
    write_bundle(CORE_OUTPUT, CORE_MODULES)
    write_bundle(BOOTSTRAP_STD_OUTPUT, BOOTSTRAP_STD_MODULES)
    write_bundle(OUTPUT, MODULES)
    run([L1CHECK, OUTPUT, "compiler_compile"])
    run([L1CHECK, CORE_OUTPUT, "compiler_compile"])
    run([L1CHECK, BOOTSTRAP_STD_OUTPUT, "lain_std_abi_version"])
    run([sys.executable, BOUNDARY_CHECK])
    write_stamp_for(CORE_OUTPUT, CORE_STAMP, core_fingerprint)
    write_stamp_for(BOOTSTRAP_STD_OUTPUT, BOOTSTRAP_STD_STAMP, std_fingerprint)
    write_bootstrap_std_manifest(std_fingerprint)
    write_stamp(fingerprint)
    print(
        f"{OUTPUT.relative_to(ROOT)} + {CORE_OUTPUT.relative_to(ROOT)} + "
        f"{BOOTSTRAP_STD_OUTPUT.relative_to(ROOT)}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"lain compiler build failed: {error}", file=sys.stderr)
        raise SystemExit(1)
