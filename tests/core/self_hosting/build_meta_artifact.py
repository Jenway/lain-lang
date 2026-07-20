#!/usr/bin/env python3
"""Build the reusable stage-1 Lain Meta compiler artifact with stage-0."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

from artifact_cache import (
    cache_matches,
    input_fingerprint as cache_fingerprint,
    write_stamp,
)
from compiler_source import MODULES, build_compiler_source


ROOT = Path(__file__).resolve().parents[3]
BOOTSTRAP_ROOT = Path(
    os.environ.get("LAIN_BOOTSTRAP_ROOT", ROOT.parent / "lain-bootstrap")
)
OUT = ROOT / "build/core-self-hosting/meta_compiler.l1"
PARTS = ROOT / "build/core-self-hosting/artifact-parts"
STAMP = ROOT / "build/core-self-hosting/meta_compiler.stamp.json"
CACHE_SCHEMA = "lain-meta-artifact-cache-v2"
COMPILER_API_SCHEMA = "1"
EXPECTED_HOST_CAPABILITIES = {
    "compiler.storage-new!",
    "compiler.storage-reserve!",
    "compiler.storage-set-i32!",
    "compiler.storage-get-i32!",
    "compiler.storage-set-string!",
    "compiler.storage-get-string!",
    "compiler.storage-destroy!",
    "core.destroy-lainir-unit!",
    "core.string-append!",
    "core.string-from-addr!",
    "core.string-first-byte!",
    "core.string-length!",
    "core.i32-to-string!",
    "core.string-is-i32!",
    "core.string-to-i32!",
    "core.string-equal!",
    "ast.node-atom-class",
    "ast.node-line",
    "ast.node-origin",
    "ast.node-hygiene",
    "ast.node-is-infix-text",
    "ast.node-is-atom-text",
    "ast.node-text",
    "ast.node-string-value",
    "ast.node-next",
    "ast.node-op",
    "ast.node-right",
    "ast.node-left",
    "ast.node-kind",
    "ast.parse!",
    "ast.store-new!",
    "ast.store-destroy!",
    "ast.store-live-count",
    "ast.unit-parse!",
    "ast.unit-new-generated!",
    "ast.unit-atom!",
    "ast.unit-node!",
    "ast.unit-set-left!",
    "ast.unit-set-right!",
    "ast.unit-set-op!",
    "ast.unit-set-origin!",
    "ast.unit-set-hygiene!",
    "ast.unit-append!",
    "ast.unit-clone!",
    "ast.unit-seal!",
    "meta.syntax-enter!",
    "meta.syntax-leave!",
    "meta.syntax-clone!",
    "meta.syntax-left!",
    "meta.syntax-right!",
    "meta.syntax-op!",
    "meta.syntax-next!",
    "meta.syntax-atom!",
    "meta.syntax-group!",
    "meta.syntax-prefix!",
    "meta.syntax-postfix!",
    "meta.syntax-infix!",
    "meta.syntax-append!",
    "meta.syntax-list-empty!",
    "meta.syntax-list-one!",
    "meta.syntax-list-two!",
    "ast.unit-destroy!",
    "ast.unit-root",
    "ast.unit-node-count",
    "ast.unit-node-kind",
    "ast.unit-node-text",
    "ast.unit-node-string-value",
    "ast.unit-node-is-atom-text",
    "ast.unit-node-is-infix-text",
    "ast.unit-node-atom-class",
    "ast.unit-node-line",
    "ast.unit-node-col",
    "ast.unit-node-left",
    "ast.unit-node-right",
    "ast.unit-node-op",
    "ast.unit-node-next",
    "l1.call-arg!",
    "l1.expr-arg!",
    "l1.expr-binary!",
    "l1.expr-call!",
    "l1.expr-alloca!",
    "l1.expr-lea!",
    "l1.expr-load!",
    "l1.expr-i32!",
    "l1.expr-string!",
    "l1.expr-var!",
    "l1.proc-if-return!",
    "l1.proc-if-return-then!",
    "l1.block-if!",
    "l1.block-else!",
    "l1.block-let!",
    "l1.block-return!",
    "l1.block-call!",
    "l1.block-store!",
    "l1.proc-let!",
    "l1.proc-new!",
    "l1.proc-extern!",
    "l1.proc-param!",
    "l1.proc-body!",
    "l1.proc-return!",
    "l1.proc-store!",
    "l1.unit-new!",
    "l1.unit-verify!",
    "l1.unit-verify-code!",
    "l1.unit-verify-message!",
    "core.lainir-unit-debug-text!",
    "l1.read-find-proc!",
    "l1.read-proc-is-extern!",
    "l1.read-proc-param-count!",
    "l1.read-proc-first-inst!",
    "l1.read-inst-next!",
    "l1.read-inst-kind!",
    "l1.read-inst-expr!",
    "l1.read-inst-name!",
    "l1.read-inst-branch!",
    "l1.read-expr-kind!",
    "l1.read-expr-i32!",
    "l1.read-expr-index!",
    "l1.read-expr-name!",
    "l1.read-expr-child!",
    "l1.read-call-arg-count!",
    "l1.read-call-arg!",
    "l1.eval-new!",
    "l1.eval-frame-new!",
    "l1.eval-frame-arg-set!",
    "l1.eval-frame-arg!",
    "l1.eval-frame-var-set!",
    "l1.eval-frame-var!",
    "l1.eval-fail!",
    "l1.eval-status!",
    "l1.eval-destroy!",
    "l1.result-new!",
    "l1.result-status!",
    "l1.result-value!",
    "l1.result-destroy!",
}


def compiler() -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out/bin" / f"lainc{suffix}"


def tool(name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out/bin" / f"{name}{suffix}"


def run(args: list[str]) -> None:
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        command = " ".join(str(arg) for arg in args)
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"command failed: {command}\n{detail}")


def input_fingerprint(source: Path) -> str:
    inputs: list[tuple[str, bytes]] = [
        ("builder", Path(__file__).read_bytes()),
        (
            "cache-contract",
            (Path(__file__).parent / "artifact_cache.py").read_bytes(),
        ),
        ("canonical-compiler-source", source.read_bytes()),
        ("stage0-lainc", compiler().read_bytes()),
        ("validator-l1check", tool("l1check").read_bytes()),
        ("schema-reader-l1i", tool("l1i").read_bytes()),
        (
            "bootstrap/polyfills.scm",
            (BOOTSTRAP_ROOT / "polyfills.scm").read_bytes(),
        ),
    ]
    inputs.extend(
        (
            "bootstrap/" + path.relative_to(BOOTSTRAP_ROOT).as_posix(),
            path.read_bytes(),
        )
        for path in sorted((BOOTSTRAP_ROOT / "std/meta").rglob("*.scm"))
    )
    return cache_fingerprint(CACHE_SCHEMA, inputs)


def compiler_artifact_valid(path: Path) -> bool:
    checked = subprocess.run(
        [str(tool("l1check")), str(path), "compiler_compile"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if checked.returncode != 0:
        return False
    schema = subprocess.run(
        [str(tool("l1i")), str(path), "compiler_api_schema_version"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    return schema.returncode == 0 and schema.stdout.strip() == COMPILER_API_SCHEMA


def build() -> Path:
    lainc = compiler()
    source = build_compiler_source()
    fingerprint = input_fingerprint(source)
    if (cache_matches(OUT, STAMP, CACHE_SCHEMA, fingerprint)
            and compiler_artifact_valid(OUT)):
        return OUT
    PARTS.mkdir(parents=True, exist_ok=True)
    for stale in PARTS.glob("*.l1"):
        stale.unlink()
    interfaces: list[Path] = []
    outputs: list[Path] = []
    try:
        for index, relative in enumerate(MODULES):
            source = ROOT / relative
            interface = ROOT / f"{relative}.lci"
            output = PARTS / f"{index:02d}-{source.stem}.l1"
            run([str(lainc), "--emit-interface", str(source), str(interface)])
            interfaces.append(interface)
            # Stage-1 must always be produced by the stage-0 reference path.
            # Normal --emit-l1 is reserved for the installed self-hosted
            # compiler artifact starting with M14.
            run([str(lainc), "--bootstrap-emit-l1", str(source), str(output)])
            outputs.append(output)
        parts = [path.read_text(encoding="utf-8") for path in outputs]
        defined = {
            match.group(1)
            for text in parts
            for match in re.finditer(r"(?m)^#proc ([^(]+)\(", text)
        }
        # Stage-0 interfaces expose imported procedures under a caller-side
        # module-qualified symbol (for example
        # `syntax_syntax_is_infix`).  A standalone artifact links
        # all of those modules into one L1 unit, so resolve that import name to
        # the concrete procedure before pruning extern declarations.
        module_names = tuple(Path(relative).stem for relative in MODULES)
        relocations: dict[str, str] = {}
        for text in parts:
            for match in re.finditer(
                r"(?:^#extern #proc |#(?:call|eval) )([^\s(]+)\(",
                text,
                re.MULTILINE,
            ):
                imported = match.group(1)
                for module_name in module_names:
                    prefix = f"{module_name}_"
                    if imported.startswith(prefix):
                        concrete = imported[len(prefix) :]
                        if concrete in defined:
                            relocations[imported] = concrete
                            break
        for imported, concrete in relocations.items():
            parts = [
                text.replace(f"#call {imported}(", f"#call {concrete}(")
                .replace(f"#eval {imported}(", f"#eval {concrete}(")
                for text in parts
            ]
        externs: set[str] = set()
        kept: list[str] = []
        for text in parts:
            for line in text.splitlines():
                match = re.match(r"^#extern #proc ([^(]+)\(", line)
                if match:
                    name = match.group(1)
                    if name in defined or name in relocations or name in externs:
                        continue
                    externs.add(name)
                kept.append(line)
        if externs != EXPECTED_HOST_CAPABILITIES:
            missing = sorted(EXPECTED_HOST_CAPABILITIES - externs)
            unexpected = sorted(externs - EXPECTED_HOST_CAPABILITIES)
            raise RuntimeError(
                "Meta artifact capability contract changed: "
                f"missing={missing}, unexpected={unexpected}"
            )
        OUT.write_text(
            "\n".join(kept) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        if not compiler_artifact_valid(OUT):
            raise RuntimeError(
                "stage-1 artifact lacks compiler_compile or compiler API schema 1"
            )
        write_stamp(OUT, STAMP, CACHE_SCHEMA, fingerprint)
        return OUT
    finally:
        for interface in interfaces:
            interface.unlink(missing_ok=True)


if __name__ == "__main__":
    print(build().relative_to(ROOT))
