#!/usr/bin/env python3
"""Build the reusable stage-1 Lain Meta compiler artifact with stage-0."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "build/core-self-hosting/m3_meta_compiler.l1"
PARTS = ROOT / "build/core-self-hosting/m3-parts"
MODULES = (
    "packages/lain/compiler/mini_syntax.lain",
    "packages/lain/compiler/mini_middle.lain",
    "packages/lain/compiler/mini_type.lain",
    "packages/lain/compiler/mini_module.lain",
    "packages/lain/compiler/mini_elaborate.lain",
    "packages/lain/compiler/l1_text_builder.lain",
    "packages/lain/compiler/mini_lower.lain",
    "packages/lain/compiler/mini_diagnostic.lain",
    "packages/lain/compiler/mini_compile_result.lain",
    "packages/lain/compiler/mini_meta.lain",
)
EXPECTED_HOST_CAPABILITIES = {
    "core.string-first-byte!",
    "core.i32-to-string!",
    "core.string-is-i32!",
    "core.string-to-i32!",
    "core.string-equal!",
    "core.string-append-linear!",
    "ast.node-atom-class",
    "ast.node-is-infix-text",
    "ast.node-is-atom-text",
    "ast.node-text",
    "ast.node-next",
    "ast.node-op",
    "ast.node-right",
    "ast.node-left",
    "ast.node-kind",
    "ast.parse!",
}


def compiler() -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return ROOT / "zig-out/bin" / f"lainc{suffix}"


def run(args: list[str]) -> None:
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def build() -> Path:
    lainc = compiler()
    PARTS.mkdir(parents=True, exist_ok=True)
    interfaces: list[Path] = []
    outputs: list[Path] = []
    try:
        for index, relative in enumerate(MODULES):
            source = ROOT / relative
            interface = ROOT / f"{relative}.lci"
            output = PARTS / f"{index:02d}-{source.stem}.l1"
            run([str(lainc), "--emit-interface", str(source), str(interface)])
            interfaces.append(interface)
            run([str(lainc), "--emit-l1", str(source), str(output)])
            outputs.append(output)
        parts = [path.read_text(encoding="utf-8") for path in outputs]
        defined = {
            match.group(1)
            for text in parts
            for match in re.finditer(r"(?m)^#proc ([^(]+)\(", text)
        }
        # Stage-0 interfaces expose imported procedures under a caller-side
        # module-qualified symbol (for example
        # `mini_syntax_mini_syntax_is_infix`).  A standalone artifact links
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
                "M3 artifact capability contract changed: "
                f"missing={missing}, unexpected={unexpected}"
            )
        OUT.write_text(
            "\n".join(kept) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        return OUT
    finally:
        for interface in interfaces:
            interface.unlink(missing_ok=True)


if __name__ == "__main__":
    print(build().relative_to(ROOT))
