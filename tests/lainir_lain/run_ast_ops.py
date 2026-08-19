#!/usr/bin/env python3
"""Exercise the Lain AST view and transform operations (seed bundle).

The RawAst layer in src/lainir/lain/raw_ast.l1 now carries the first
semantic views and structural transforms on top of its topology-only tree:

  - ast_is_call / ast_postfix_name / ast_postfix_args: the `Name(args)`
    postfix-group view shared by calls, type applications and effects;
  - ast_is_atom / ast_span_equal: text predicates over node spans;
  - ast_node_count: iterative subtree counting (explicit stack, no
    recursion);
  - ast_replace_child: replace a child in a sibling chain (the
    replacement takes over the old child's next link);
  - ast_copy: deep copy of a subtree;
  - ast_write: AST -> source text (atoms emit their bytes, groups emit
    delimiters and children);
  - ast_from_text: parse a source span into a fresh, independent AST
    (re-based to parent-source coordinates) — the text -> AST generation
    direction.

The probe entry parses `let x = foo(1, 2);`, replaces the first argument
with a copy of the second, writes the transformed call back as text,
re-parses the original call text into a fresh AST, and reports each
operation's result as `key:value` pairs.  This runner asserts the exact
expected line.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SEED = ROOT / "seed" / "zig-out" / "bin" / "lainir-seed.exe"
CHECK = ROOT / "seed" / "zig-out" / "bin" / "lainir-print.exe"
BUNDLER = ROOT / "scripts" / "bundle_lainir.py"
TOOLS = ROOT / "src" / "lainir" / "tools"
PARSER = ROOT / "src" / "lainir" / "lain" / "raw_ast.l1"


def run(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-ast-ops-") as temporary:
        directory = pathlib.Path(temporary)
        bundle = directory / "ast_ops_bundle.l1"
        output = directory / "ast_ops.txt"
        fixture = directory / "fixture.lain"
        fixture.write_text("let x = foo(1, 2);\n", encoding="utf-8")

        bundled = run(
            [
                sys.executable,
                str(BUNDLER),
                "-o",
                str(bundle),
                str(TOOLS / "source.l1"),
                str(PARSER),
            ]
        )
        if bundled.returncode:
            print(bundled.stderr or bundled.stdout, file=sys.stderr)
            return 1
        checked = run([str(CHECK), str(bundle), "lain_ast_ops_probe"])
        if checked.returncode:
            print(checked.stderr or checked.stdout, file=sys.stderr)
            return 1
        executed = run(
            [
                str(SEED),
                str(bundle),
                "lain_ast_ops_probe",
                str(output),
                str(fixture),
            ]
        )
        if executed.returncode:
            print(executed.stderr or executed.stdout, file=sys.stderr)
            return 1
        text = output.read_text(encoding="utf-8").strip()
        # call:1 name:foo args:4 nodes:9 replaced:1 after:4 text:foo(2,2) copy:4
        parts = dict(item.split(":", 1) for item in text.split())
        expected = {
            "call": "1",       # foo(1,2) is a call-shaped postfix group
            "name": "foo",     # postfix name is `foo`
            "args": "4",       # paren group has 4 nodes (foo,1,',',2)
            "nodes": "9",      # whole tree: let x = foo(1, 2) ;
            "replaced": "1",   # first arg replaced
            "after": "4",      # args group still foo,2,',',2 = 4 nodes
            "text": "foo(2,2)",  # transformed call written back as text
            "copy": "4",       # deep copy preserves the 4-node group
            "gen": "6",        # fresh parse of foo(1,2): root+foo+group(3)
            "gentext": "foo(1,2)",  # generated AST writes back unchanged
        }
        for key, value in expected.items():
            if parts.get(key) != value:
                print(
                    f"ast-ops mismatch: {key!r} = {parts.get(key)!r}, "
                    f"expected {value!r}; full line {text!r}",
                    file=sys.stderr,
                )
                return 1
        # Macro instantiation: `twice(21)` expands the template `(x + x)`
        # by substituting each `x` placeholder with a copy of the argument.
        macro_fixture = directory / "macro_fixture.lain"
        macro_fixture.write_text(
            "let y = twice(21);\nlet t = (x + x);\n", encoding="utf-8"
        )
        macro_output = directory / "macro.txt"
        macro_run = run(
            [
                str(SEED),
                str(bundle),
                "lain_macro_expand_probe",
                str(macro_output),
                str(macro_fixture),
            ]
        )
        if macro_run.returncode:
            print(macro_run.stderr or macro_run.stdout, file=sys.stderr)
            return 1
        macro_text = macro_output.read_text(encoding="utf-8").strip()
        macro_parts = dict(item.split(":", 1) for item in macro_text.split())
        macro_expected = {
            "tmpl": "(x+x)",      # template group text
            "args": "2",          # argument `21` tree has 2 nodes (root+atom)
            "first_x": "1",       # first placeholder found
            "inst": "(21+21)",    # instantiated template text
        }
        for key, value in macro_expected.items():
            if macro_parts.get(key) != value:
                print(
                    f"macro mismatch: {key!r} = {macro_parts.get(key)!r}, "
                    f"expected {value!r}; full line {macro_text!r}",
                    file=sys.stderr,
                )
                return 1
        # Macro declaration: `let twice = macro(x) { (x + x) };` is
        # recognised, its template extracted, and the call site `twice(21)`
        # expanded by substituting the argument for the parameter.
        decl_fixture = directory / "decl_fixture.lain"
        decl_fixture.write_text(
            "let twice = macro(x) { (x + x) };\n"
            "let y = twice(21);\n",
            encoding="utf-8",
        )
        decl_output = directory / "decl.txt"
        decl_run = run(
            [
                str(SEED),
                str(bundle),
                "lain_macro_decl_probe",
                str(decl_output),
                str(decl_fixture),
            ]
        )
        if decl_run.returncode:
            print(decl_run.stderr or decl_run.stdout, file=sys.stderr)
            return 1
        decl_text = decl_output.read_text(encoding="utf-8").strip()
        decl_parts = dict(item.split(":", 1) for item in decl_text.split())
        decl_expected = {
            "name": "twice",     # macro name from declaration
            "param": "x",        # macro parameter
            "tmpl": "(x+x)",     # extracted template group
            "arg": "21",         # call-site argument
            "inst": "(21+21)",   # expanded template
        }
        for key, value in decl_expected.items():
            if decl_parts.get(key) != value:
                print(
                    f"macro-decl mismatch: {key!r} = {decl_parts.get(key)!r}, "
                    f"expected {value!r}; full line {decl_text!r}",
                    file=sys.stderr,
                )
                return 1
        # Program-level expansion: the macro declaration is removed from
        # the tree and the call site is replaced with the expansion, then
        # the whole program is written back.
        prog_fixture = directory / "prog_fixture.lain"
        prog_fixture.write_text(
            "let twice = macro(x) { (x + x) };\n"
            "let y = twice(21);\n",
            encoding="utf-8",
        )
        prog_output = directory / "prog.txt"
        prog_run = run(
            [
                str(SEED),
                str(bundle),
                "lain_macro_program_probe",
                str(prog_output),
                str(prog_fixture),
            ]
        )
        if prog_run.returncode:
            print(prog_run.stderr or prog_run.stdout, file=sys.stderr)
            return 1
        prog_text = prog_output.read_text(encoding="utf-8").strip()
        expected_prog = "prog:lety=(21+21);"
        if prog_text != expected_prog:
            print(
                f"program-expansion mismatch: {prog_text!r}, "
                f"expected {expected_prog!r}",
                file=sys.stderr,
            )
            return 1
        # Multi call-site expansion: every `twice(...)` call is expanded.
        multi_fixture = directory / "multi_fixture.lain"
        multi_fixture.write_text(
            "let twice = macro(x) { (x + x) };\n"
            "let a = twice(21);\n"
            "let b = twice(5);\n"
            "let c = twice(1);\n",
            encoding="utf-8",
        )
        multi_output = directory / "multi.txt"
        multi_run = run(
            [
                str(SEED),
                str(bundle),
                "lain_macro_multi_probe",
                str(multi_output),
                str(multi_fixture),
            ]
        )
        if multi_run.returncode:
            print(multi_run.stderr or multi_run.stdout, file=sys.stderr)
            return 1
        multi_text = multi_output.read_text(encoding="utf-8").strip()
        expected_multi = "prog:leta=(21+21);letb=(5+5);letc=(1+1); expanded:3"
        if multi_text != expected_multi:
            print(
                f"multi-expansion mismatch: {multi_text!r}, "
                f"expected {expected_multi!r}",
                file=sys.stderr,
            )
            return 1
        # Nested macro expansion: `twice`'s template contains `inc` calls,
        # which are expanded after `twice` itself.
        nested_fixture = directory / "nested_fixture.lain"
        nested_fixture.write_text(
            "let inc = macro(x) { (x + 1) };\n"
            "let twice = macro(x) { (inc(x) + inc(x)) };\n"
            "let y = twice(21);\n",
            encoding="utf-8",
        )
        nested_output = directory / "nested.txt"
        nested_run = run(
            [
                str(SEED),
                str(bundle),
                "lain_macro_nested_probe",
                str(nested_output),
                str(nested_fixture),
            ]
        )
        if nested_run.returncode:
            print(nested_run.stderr or nested_run.stdout, file=sys.stderr)
            return 1
        nested_text = nested_output.read_text(encoding="utf-8").strip()
        expected_nested = "prog:lety=((21+1)+(21+1)); expanded:3 residual:0"
        if nested_text != expected_nested:
            print(
                f"nested-expansion mismatch: {nested_text!r}, "
                f"expected {expected_nested!r}",
                file=sys.stderr,
            )
            return 1
        # Round-trip: the expanded program (no macro calls left) must be
        # parseable as ordinary Lain source.  Feed the `prog:` text back
        # through the raw parser and require a successful dump.
        expanded_prog = nested_text.split("prog:", 1)[1].split(" ", 1)[0]
        roundtrip_fixture = directory / "roundtrip.lain"
        roundtrip_fixture.write_text(expanded_prog + "\n", encoding="utf-8")
        roundtrip_output = directory / "roundtrip.txt"
        roundtrip_run = run(
            [
                str(SEED),
                str(bundle),
                "lain_raw_ast_dump",
                str(roundtrip_output),
                str(roundtrip_fixture),
            ]
        )
        if roundtrip_run.returncode:
            print(
                f"round-trip parse failed for {expanded_prog!r}: "
                f"{roundtrip_run.stderr or roundtrip_run.stdout}",
                file=sys.stderr,
            )
            return 1
        roundtrip_text = roundtrip_output.read_text(encoding="utf-8").strip()
        if not roundtrip_text.startswith("(root (atom lety)"):
            print(
                f"round-trip dump unexpected: {roundtrip_text!r}",
                file=sys.stderr,
            )
            return 1
    print("PASS Lain AST views, transforms, macro expansion incl. nested + round-trip (seed bundle)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
