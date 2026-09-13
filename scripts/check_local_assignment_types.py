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
def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path)
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
        for label, text in CASES.items():
            source = work / (label + ".lain"); source.write_text(text + "\n", encoding="utf-8")
            output = work / (label + ".l1")
            compiled = subprocess.run([seed_exe("lainir-seed"), compiler, "compiler_compile_library", output, source,
                                       ROOT / "scripts/fixtures/empty_source.lain"], cwd=ROOT, capture_output=True, text=True, timeout=40)
            if compiled.returncode:
                raise RuntimeError(f"{label}: compile failed: {compiled.stderr}")
            verified = subprocess.run([seed_exe("lainir-print"), output, "main"], cwd=ROOT, capture_output=True, text=True)
            if verified.returncode:
                raise RuntimeError(f"{label}: verifier rejected output: {verified.stderr}")
            runtime = work / (label + "-run.l1")
            subprocess.run([sys.executable, ROOT / "scripts/bundle_lainir.py", "-o", runtime, output, probe], cwd=ROOT, capture_output=True, check=True)
            result = work / (label + ".txt")
            executed = subprocess.run([seed_exe("lainir-seed"), "interpreter", runtime, "local_type_probe", result, source,
                                       ROOT / "scripts/fixtures/empty_source.lain"], cwd=ROOT, capture_output=True, text=True, timeout=40)
            if executed.returncode or not result.is_file() or result.read_text().strip() != "42":
                raise RuntimeError(f"{label}: expected execution result 42: {executed.stderr}")
            print(f"PASS lexical assignment {label}: 42")
    return 0
if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
