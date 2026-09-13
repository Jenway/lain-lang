#!/usr/bin/env python3
"""Prove library AST/semantic stage results are consumed without core changes."""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

from check_meta_pipeline_audit import proc_region
from toolchain import seed_exe

ROOT = Path(__file__).resolve().parents[1]
SEED = seed_exe("lainir-seed")
PRINT = seed_exe("lainir-print")
SOURCE = """let replacement = std::func() -> i64 { return 7; };
let main = std::func() -> i64 { return 42; };
"""
HELPERS = r'''
#proc audit_rewrite_ast(#addr %index) -> #unit {
  #let %entry: #addr = #call syntax_index_entry(%index, 0)
  #let %root: #addr = #call syntax_unit_root(%entry)
  #let %cursor: #addr = #call lain_ast_v1_first_child(%root)
  #let %first: #addr = #call lain_ast_v1_nil()
  #loop bodies {
    #if #call lain_ast_v1_is_nil(%cursor) { #return }
    #if #eq(#call lain_ast_v1_node_delimiter(%cursor), 123) {
      #if #call lain_ast_v1_is_nil(%first) {
        %first: #addr = %cursor
      } else {
        #let %copy: #addr = #call lain_ast_v1_copy(%first)
        #call lain_ast_v1_attach_origin(%copy, #call syntax_unit_source(%entry),
          #call lain_ast_v1_node_start(%first), #call lain_ast_v1_node_length(%first))
        #call lain_ast_v1_set_syntax_context(%copy, 991)
        #let %changed: #bits<1> = #call lain_ast_v1_replace(%root, %cursor, %copy)
        #return
      }
    }
    %cursor: #addr = #call lain_ast_v1_next_sibling(%cursor)
    #continue bodies
  }
}
#proc audit_rewrite_semantics(#addr %unit) -> #unit {
  #let %first: #addr = #call program_unit_head(%unit)
  #let %main: #addr = #load[#addr](#lea(base=%first, idx=0, scale=0, offset=40))
  #let %copy: #addr = #call lain_ast_v1_copy(#call program_function_body(%first))
  #store %copy, #lea(base=%main, idx=0, scale=0, offset=32)
  #return
}
#proc audit_check_metadata(#addr %unit) -> #bits<1> {
  #let %first: #addr = #call program_unit_head(%unit)
  #let %main: #addr = #load[#addr](#lea(base=%first, idx=0, scale=0, offset=40))
  #let %body: #addr = #call program_function_body(%main)
  #let %original: #addr = #call program_function_body(%first)
  #if #ne(#call lain_ast_v1_node_syntax_context(%body), 991) { #return 0 }
  #if #eq(#call lain_ast_v1_node_origin_present(%body), 0) { #return 0 }
  #if #ne(#ptr2int(#call lain_ast_v1_node_origin_source(%body)),
          #ptr2int(#call program_function_source(%main))) { #return 0 }
  #if #ne(#call lain_ast_v1_node_origin_start(%body),
          #call lain_ast_v1_node_start(%original)) { #return 0 }
  #if #ne(#call lain_ast_v1_node_origin_length(%body),
          #call lain_ast_v1_node_length(%original)) { #return 0 }
  #return 1
}
#proc audit_emit_constant(#addr %unit) -> #bits<32> {
  #call bootstrap.artifact-begin()
  #call bootstrap.artifact-write-literal("#proc main() -> #bits<64> { #return 13 }\n")
  #call bootstrap.artifact-finish()
  #call lain_std_release_program_vectors(%unit)
  #return 0
}
'''


def run(*args: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(x) for x in args], cwd=ROOT, capture_output=True, text=True)


def require(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode:
        raise RuntimeError(f"{label}: {result.stderr.strip() or result.stdout.strip()}")


def change(text: str, name: str, old: str, new: str) -> str:
    a, b = proc_region(text, name)
    body = text[a:b]
    if body.count(old) != 1:
        raise RuntimeError(f"{name}: expected exactly one mutation point")
    return text[:a] + body.replace(old, new) + text[b:]


def main() -> int:
    require(run(sys.executable, ROOT / "scripts" / "build_lain_compiler.py"), "build")
    bundle = (ROOT / "build" / "bootstrap" / "lainc.l1").read_text(encoding="utf-8")
    core = (ROOT / "build" / "bootstrap" / "compiler_core.l1").read_text(encoding="utf-8")
    elaborate = "  #let %unit: #addr = #call lain_std_elaborate_program(%owner, %expanded_root)"
    expanded = change(bundle, "lain_std_expand", "  #let %owner: #addr =",
                      "  #call audit_rewrite_ast(%source_unit)\n  #let %owner: #addr =")
    expanded = change(expanded, "lain_std_lower", "  #let %owner: #addr =",
                      "  #if #eq(#call audit_check_metadata(%elaborated_root), 0) {\n"
                      "    #return #call lain_meta_pass_result_v1_new(5104, 0, %l1_unit,\n"
                      "      #call lain_ast_v1_nil(), #call lain_ast_v1_nil(),\n"
                      "      #call lain_compile_context_v1_owner(%context))\n  }\n  #let %owner: #addr =")
    semantic = change(bundle, "lain_std_elaborate", elaborate,
                      elaborate + "\n  #call audit_rewrite_semantics(%unit)")
    lowered = change(bundle, "lain_std_lower", "#call lain_std_emit_program(%elaborated_root)",
                     "#call audit_emit_constant(%elaborated_root)")
    stopped = change(bundle, "lain_std_lower", "  #let %owner: #addr =",
                     "  #let %audit_trap: #bits<64> = #sdiv(1, 0)\n  #let %owner: #addr =")
    cases = (
        ("baseline", bundle, "42"),
        ("expand AST + origin/hygiene through lower", expanded + HELPERS, "7"),
        ("elaborate payload changes lower", semantic + HELPERS, "7"),
        ("library lower changes IR", lowered + HELPERS, "13"),
    )
    with tempfile.TemporaryDirectory(prefix="meta-stages-", dir=ROOT / "build") as raw:
        work = Path(raw)
        source = work / "stages.lain"
        source.write_text(SOURCE, encoding="utf-8", newline="\n")
        for index, (label, text, expected) in enumerate(cases):
            # All modifications are in library procedures; core bodies stay byte-identical.
            for name in ("lain_std_expand", "lain_std_elaborate", "lain_std_lower"):
                if re.search(r"^#proc " + re.escape(name) + r"\(", core, re.M):
                    raise RuntimeError(f"{name} incorrectly implemented in core")
            for name in re.findall(r"^#proc ([^\s(]+)", core, re.M):
                a, b = proc_region(bundle, name)
                c, d = proc_region(text, name)
                if bundle[a:b] != text[c:d]:
                    raise RuntimeError(f"{label}: changed core procedure {name}")
            compiler = work / f"compiler-{index}.l1"
            compiler.write_text(text, encoding="utf-8", newline="\n")
            require(run(PRINT, compiler, "compiler_compile_library"), label + " compiler verify")
            artifact = work / f"result-{index}.l1"
            require(run(SEED, "interpreter", compiler, "compiler_compile_library", artifact,
                        source, ROOT / "scripts" / "fixtures" / "empty_source.lain"), label + " compile")
            require(run(PRINT, artifact, "main"), label + " artifact verify")
            result = run(SEED, "run", artifact, "main")
            require(result, label + " run")
            if result.stdout.strip() != expected:
                raise RuntimeError(f"{label}: expected {expected}, got {result.stdout.strip()!r}")
            print(f"PASS {label}: {expected}", flush=True)
        # A real semantic error must terminate at elaborate, before the trapping lower.
        compiler = work / "stopped-lower.l1"
        compiler.write_text(stopped, encoding="utf-8", newline="\n")
        invalid = work / "invalid.lain"
        invalid.write_text("let main = std::func() -> bogus { return 42; };\n", encoding="utf-8")
        artifact = work / "invalid.l1"
        require(run(PRINT, compiler, "compiler_compile_library"), "stopped lower verify")
        failed = run(SEED, "interpreter", compiler, "compiler_compile_library", artifact,
                     invalid, ROOT / "scripts" / "fixtures" / "empty_source.lain")
        if not failed.returncode or "5104" not in failed.stderr + failed.stdout:
            raise RuntimeError(f"elaborate error not propagated: {failed.stderr}")
        if "sdiv by zero" in failed.stderr:
            raise RuntimeError("lower ran after elaborate failed")
        diagnostic = artifact.read_text(encoding="utf-8")
        if not diagnostic.startswith("(error 5104"):
            raise RuntimeError("semantic failure did not preserve diagnostic artifact")
        expected_start = invalid.read_text(encoding="utf-8").index("bogus")
        if f"(context node bogus start={expected_start} " not in diagnostic:
            raise RuntimeError(f"elaborate did not preserve the diagnostic source span: {diagnostic}")
        print("PASS elaborate diagnostic 5104 stops before lower", flush=True)
    print("PASS library Meta stage swap; core unchanged")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError) as error:
        print(f"FAIL Meta stage swap: {error}", file=sys.stderr)
        raise SystemExit(1)
