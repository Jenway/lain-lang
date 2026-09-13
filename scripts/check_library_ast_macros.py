#!/usr/bin/env python3
"""Check macro helper ownership and execute library expansion probes.

These legacy probes exercise helpers, not the default Meta expand ABI.  They
do not prove that arbitrary user macros work in the compiler pipeline.
"""
from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

from check_meta_pipeline_audit import proc_region
from toolchain import seed_exe

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = "prog:lety=((21+1)+(21+1)); expanded:3 residual:0\n"
SOURCE = """let inc = macro(x) { (x + 1) };
let twice = macro(x) { (inc(x) + inc(x)) };
let y = twice(21);
"""


def run(*args: str | Path) -> None:
    result = subprocess.run(list(map(str, args)), cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr or result.stdout)


def main() -> int:
    run(sys.executable, ROOT / "scripts/build_lain_compiler.py")
    core = (ROOT / "build/bootstrap/compiler_core.l1").read_text(encoding="utf-8")
    stdlib = (ROOT / "build/bootstrap/stdlib.l1").read_text(encoding="utf-8")
    if re.search(r"^#proc (?:lain_macro_|ast_\w*macro|ast_arg_expr_end|ast_substitute_params|lain_ast_all_probe)", core, re.M):
        raise RuntimeError("macro helpers remain in compiler core")
    for name in ("ast_macro_param", "ast_expand_macro_call", "ast_substitute_params", "lain_macro_nested_probe"):
        if not re.search(r"^#proc " + name + r"\(", stdlib, re.M):
            raise RuntimeError("library helper missing: " + name)
    # Check negative controls without mutating the authoritative artifact.
    from check_lainir_boundaries import FORBIDDEN_PROC, FORBIDDEN_LITERAL
    assert FORBIDDEN_PROC.search("#proc lain_macro_nested_probe() -> #bits<32> {")
    assert FORBIDDEN_PROC.search("#proc ast_expand_macro_call(#addr %x) -> #addr {")
    assert FORBIDDEN_LITERAL.search('"macro"')
    bundle = (ROOT / "build/bootstrap/lainc.l1").read_text(encoding="utf-8")
    a, b = proc_region(bundle, "lain_macro_nested_probe")
    body = bundle[a:b]
    guard = "#if #ne(#call bootstrap.source-count(), 1) { #return 2 }"
    if body.count(guard) != 1:
        raise RuntimeError("unexpected legacy probe source guard")
    # A single source invokes the host's IR adapter.  Accept its two-source
    # compiler mode in this temporary probe, still reading only source zero.
    body = body.replace(guard, "#if #ult(#call bootstrap.source-count(), 1) { #return 2 }")
    with tempfile.TemporaryDirectory(prefix="library-ast-macros-") as directory:
        temp = Path(directory)
        source = temp / "nested.lain"
        source.write_text(SOURCE, encoding="utf-8")
        probe = temp / "probe.l1"
        probe.write_text(bundle[:a] + body + bundle[b:], encoding="utf-8")
        output = temp / "expanded.txt"
        run(seed_exe("lainir-print"), probe, "lain_macro_nested_probe")
        run(seed_exe("lainir-seed"), probe, "lain_macro_nested_probe", output,
            source, ROOT / "scripts/fixtures/empty_source.lain")
        if output.read_text(encoding="utf-8") != EXPECTED:
            raise RuntimeError("nested library expansion differs: " + output.read_text())
        # Change only the library substitution procedure.  Keeping each
        # placeholder instead of inserting its argument must change the
        # expanded AST, while the same core remains in the bundle.
        text = probe.read_text(encoding="utf-8")
        start, end = proc_region(text, "ast_expand_macro_call")
        old = "#call ast_substitute(%source, %body, %ph, %plen, %arg_tree)"
        if text[start:end].count(old) != 1:
            raise RuntimeError("unexpected substitution implementation")
        changed = text[:start] + text[start:end].replace(old, "// Test replacement retains placeholders.") + text[end:]
        for name in re.findall(r"^#proc ([\w.-]+)\(", core, re.M):
            left, right = proc_region(text, name)
            new_left, new_right = proc_region(changed, name)
            if text[left:right] != changed[new_left:new_right]:
                raise RuntimeError("library replacement changed core: " + name)
        replacement = temp / "replacement.l1"
        replacement.write_text(changed, encoding="utf-8")
        run(seed_exe("lainir-print"), replacement, "lain_macro_nested_probe")
        run(seed_exe("lainir-seed"), replacement, "lain_macro_nested_probe", output,
            source, ROOT / "scripts/fixtures/empty_source.lain")
        if output.read_text(encoding="utf-8") != EXPECTED.replace("21", "x"):
            raise RuntimeError("library substitution replacement did not change expansion")
    print("PASS library macro ownership, boundary negative controls, nested expansion and library substitution swap")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
