#!/usr/bin/env python3
"""Verify and execute assignments with lexical binding types in ordinary Lain."""
from __future__ import annotations
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path
from toolchain import seed_exe
ROOT = Path(__file__).resolve().parents[1]
ALLOCATE = '@foreign(c, link_name = "bootstrap.allocate-pages")\nlet allocate = std::func(size: u64) -> addr;\n'
CASES = {
    "conditional_addr": ALLOCATE + """let identity = std::func(flag: bool, node: addr) -> addr {
    if flag { let cursor: addr = node; cursor = node; return cursor; }
    return node;
};
let main = std::func() -> i64 {
    let pointer: addr = allocate(8);
    let result: addr = identity(true, pointer);
    if result == pointer { return 42; } return 0;
};""",
    "loop_addr": ALLOCATE + """let main = std::func() -> i64 {
    let pointer: addr = allocate(8); let original: addr = pointer; let count: i32 = 0;
    while count < 1 {
        let cursor: addr = pointer; cursor = original; pointer = cursor; count = count + 1;
    }
    if count == 1 && pointer == original { return 42; } return 0;
};""",
    "shadow_restore": ALLOCATE + """let main = std::func() -> i64 {
    let cursor: i32 = 41; let pointer: addr = allocate(8);
    if true { let cursor: addr = pointer; cursor = pointer; }
    cursor = cursor + 1;
    if cursor == 42 { return 42; } return 0;
};""",
    "later_declaration": ALLOCATE + """let main = std::func() -> i64 {
    let cursor: addr = allocate(8); let original: addr = cursor;
    if true { cursor = original; let cursor: i32 = 0; }
    if cursor == original { return 42; } return 0;
};""",
    "parameter_addr": ALLOCATE + """let identity = std::func(node: addr) -> addr { node = node; return node; };
let main = std::func() -> i64 {
    let pointer: addr = allocate(8); let result: addr = identity(pointer);
    if result == pointer { return 42; } return 0;
};""",
    "inferred_addr": ALLOCATE + """let identity = std::func(node: addr) -> addr { return node; };
let main = std::func() -> i64 {
    let pointer: addr = allocate(8); let cursor = identity(pointer); cursor = pointer;
    if cursor == pointer { return 42; } return 0;
};""",
    "conditional_bool": """let main = std::func() -> i64 {
    if true { let flag: bool = false; flag = true; if flag { return 42; } }
    return 0;
};""",
}
CASES["initializer_shadow"] = """let main = std::func() -> i64 {
    let value: i32 = 41;
    if true { let value: i32 = value + 1; if value == 42 { return 42; } }
    return 0;
};"""
CASES["parameter_loop_update"] = """let advance = std::func(value: i32) -> i32 {
    let count: i32 = 0;
    while count < 2 { value = value + 1; count = count + 1; }
    return value;
};
let main = std::func() -> i64 { if advance(40) == 42 { return 42; } return 0; };"""
CASES["arithmetic_boolean_precedence"] = """let main = std::func() -> i64 {
    let index: usize = 1; let limit: usize = 3; let byte: i32 = 47;
    if byte == 47 && index + 1 < limit && byte == 47 { return 42; }
    return 0;
};"""
CASES["record_local"] = """let Point: std::type = std::struct { x: i64 };
let main = std::func() -> i64 { let point: Point = Point { x: 41 }; point.x = 42; return point.x; };"""
CASES["record_parameter"] = """let Point: std::type = std::struct { x: i64 };
let read = std::func(point: &Point) -> i64 { return point.x; };
let main = std::func() -> i64 { let point: Point = Point { x: 42 }; return read(&point); };"""
CASES["record_shadow_layout"] = """let Outer: std::type = std::struct { x: i64 };
let Inner: std::type = std::struct { y: i32 };
let main = std::func() -> i64 {
    let point: Outer = Outer { x: 41 };
    if true { let point: Inner = Inner { y: 0 }; point.y = 42; if point.y != 42 { return 0; } }
    point.x = 42; return point.x;
};"""
CASES["macro_generated_locals"] = """let step = macro(target) { let count: i64 = 1; target = target + count; };
let main = std::func() -> i64 { let result: i64 = 40; step(result) step(result) return result; };"""
CASES["parameter_generated_name_collision"] = """let combine = std::func(value: i64, value__parameter: i64) -> i64 {
    return value + value__parameter;
};
let main = std::func() -> i64 { return combine(20, 22); };"""
PROBE = """#extern #proc main() -> #bits<64>;
#extern #proc bootstrap.artifact-begin() -> #unit;
#extern #proc bootstrap.artifact-write-literal(#addr %text) -> #unit;
#extern #proc bootstrap.artifact-finish() -> #unit;
#proc local_type_probe() -> #bits<32> {
  #if #ne(#call main(), 42) { #return 1 }
  #call bootstrap.artifact-begin()
  #call bootstrap.artifact-write-literal("42\\n")
  #call bootstrap.artifact-finish()
  #return 0
}
"""

SCOPE_ERRORS = {
    "undeclared_uppercase": "let main = std::func() -> i64 { return X; };",
    "escaped_read": "let main = std::func() -> i64 { if true { let value:i64=42; } return value; };",
    "sibling_read": "let main = std::func() -> i64 { if true { let value:i64=42; } if true { return value; } return 0; };",
    "forward_read": "let main = std::func() -> i64 { let first:i64=value; let value:i64=42; return first; };",
    "nested_initializer": "let main = std::func() -> i64 { if true { let first:i64=value; } return 0; };",
    "nested_loop_condition": "let main = std::func() -> i64 { if true { while value < 1 { return 0; } } return 0; };",
    "escaped_record_field": "let Point:std::type=std::struct{x:i64}; let main=std::func()->i64 { if true { let point:Point=Point{x:42}; } return point.x; };",
    "escaped_address": "let main=std::func()->i64 { if true { let value:i64=42; } let pointer:addr = &value; return 0; };",
    "escaped_condition": "let main=std::func()->i64 { if true { let value:i64=42; } if value==42 { return 42; } return 0; };",
}
def main(cases: dict[str, str] | None = None, label: str = "lexical assignment") -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--deterministic", action="store_true", help="compare two independent compiler runs")
    parser.add_argument("--check-scope-errors", action="store_true", help="require semantic rejection of out-of-scope reads")
    args = parser.parse_args()
    if args.compiler is None:
        subprocess.run([sys.executable, ROOT / "scripts/build_lain_compiler.py"], cwd=ROOT, check=True, capture_output=True)
    compiler = (args.compiler or ROOT / "build/bootstrap/lainc.l1").resolve()
    if not compiler.is_file():
        print(f"compiler bundle not found: {compiler}", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="local-assignment-", dir=ROOT / "build") as directory:
        work = Path(directory)
        probe = work / "probe.l1"; probe.write_text(PROBE, encoding="utf-8")
        for case_name, text in (CASES if cases is None else cases).items():
            source = work / (case_name + ".lain"); source.write_text(text + "\n", encoding="utf-8")
            output = work / (case_name + ".l1")
            compiled = subprocess.run([seed_exe("lainir-seed"), compiler, "compiler_compile_library", output, source,
                                       ROOT / "scripts/fixtures/empty_source.lain"], cwd=ROOT, capture_output=True, text=True, timeout=40)
            if compiled.returncode:
                raise RuntimeError(f"{case_name}: compile failed: {compiled.stderr}")
            if args.deterministic:
                repeated = work / (case_name + "-repeat.l1")
                again = subprocess.run([seed_exe("lainir-seed"), compiler, "compiler_compile_library", repeated, source,
                                        ROOT / "scripts/fixtures/empty_source.lain"], cwd=ROOT, capture_output=True, text=True, timeout=40)
                if again.returncode or repeated.read_bytes() != output.read_bytes():
                    raise RuntimeError(f"{case_name}: independent compiler runs differ: {again.stderr}")
            verified = subprocess.run([seed_exe("lainir-print"), output, "main"], cwd=ROOT, capture_output=True, text=True)
            if verified.returncode:
                raise RuntimeError(f"{case_name}: verifier rejected output: {verified.stderr}")
            runtime = work / (case_name + "-run.l1")
            subprocess.run([sys.executable, ROOT / "scripts/bundle_lainir.py", "-o", runtime, output, probe], cwd=ROOT, capture_output=True, check=True)
            result = work / (case_name + ".txt")
            executed = subprocess.run([seed_exe("lainir-seed"), "interpreter", runtime, "local_type_probe", result, source,
                                       ROOT / "scripts/fixtures/empty_source.lain"], cwd=ROOT, capture_output=True, text=True, timeout=40)
            if executed.returncode or not result.is_file() or result.read_text().strip() != "42":
                raise RuntimeError(f"{case_name}: expected execution result 42: {executed.stderr}")
            print(f"PASS {label} {case_name}: 42")
        if args.check_scope_errors:
            for case_name, text in SCOPE_ERRORS.items():
                source = work / (case_name + ".lain")
                source.write_text(text + "\n", encoding="utf-8")
                output = work / (case_name + ".l1")
                rejected = subprocess.run(
                    [seed_exe("lainir-seed"), compiler, "compiler_compile_library", output, source,
                     ROOT / "scripts/fixtures/empty_source.lain"],
                    cwd=ROOT, capture_output=True, text=True, timeout=40,
                )
                # The direct seed ABI saves diagnostics in the requested output;
                # require a semantic diagnostic, never a nominal IR artifact.
                if rejected.returncode == 0 or not output.is_file() or not output.read_text(encoding="utf-8").startswith("(error 5108)\n"):
                    raise RuntimeError(f"{case_name}: expected semantic diagnostic 5108: {rejected.stderr}")
                print(f"PASS lexical scope rejection {case_name}: 5108")
    return 0
if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
