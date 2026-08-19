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
    delimiters and children).

The probe entry parses `let x = foo(1, 2);`, replaces the first argument
with a copy of the second, writes the transformed call back as text and
reports each operation's result as `key:value` pairs.  This runner
asserts the exact expected line.
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
        }
        for key, value in expected.items():
            if parts.get(key) != value:
                print(
                    f"ast-ops mismatch: {key!r} = {parts.get(key)!r}, "
                    f"expected {value!r}; full line {text!r}",
                    file=sys.stderr,
                )
                return 1
    print("PASS Lain AST views and transforms (seed bundle)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
