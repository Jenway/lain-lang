#!/usr/bin/env python3
"""RawAst topology golden tests."""

from __future__ import annotations

import difflib
import os
import pathlib
import shutil
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[3]
THIS = pathlib.Path(__file__).resolve().parent
FIXTURES = THIS / "fixtures"
BUILD = ROOT / "target" / "core-tests"


def find_c_compilers() -> list[list[str]]:
    compilers: list[list[str]] = []
    zig = shutil.which("zig")
    if zig:
        compilers.append([zig, "cc"])
    for name in ("clang", "gcc", "cc"):
        path = shutil.which(name)
        if path:
            compilers.append([path])
    return compilers


def build_dump_tool() -> pathlib.Path:
    compilers = find_c_compilers()
    if not compilers:
        raise RuntimeError("no C compiler found for AST golden tests")

    BUILD.mkdir(parents=True, exist_ok=True)
    zig_local_cache = ROOT / "target" / "zig-cache" / "local"
    zig_global_cache = ROOT / "target" / "zig-cache" / "global"
    zig_local_cache.mkdir(parents=True, exist_ok=True)
    zig_global_cache.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.setdefault("ZIG_LOCAL_CACHE_DIR", str(zig_local_cache))
    env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(zig_global_cache))

    exe = BUILD / ("ast_dump.exe" if sys.platform == "win32" else "ast_dump")
    failures: list[str] = []
    for compiler in compilers:
        command = [
            *compiler,
            "-std=c11",
            "-I",
            str(ROOT / "src"),
            str(THIS / "ast_dump.c"),
            str(ROOT / "src/lainast/lain_ast.c"),
            str(ROOT / "src/lainast/lain_ast_parser.c"),
            "-o",
            str(exe),
        ]
        result = subprocess.run(
            command, cwd=ROOT, env=env, capture_output=True, text=True
        )
        if result.returncode == 0:
            return exe
        message = result.stderr.strip() or result.stdout.strip()
        failures.append(f"{' '.join(compiler)}: {message}")

    raise RuntimeError("all C compiler candidates failed:\n" + "\n".join(failures))


def normalize(text: str) -> str:
    return text.replace("\r\n", "\n").rstrip() + "\n"


def compiler_path() -> pathlib.Path:
    exe = ROOT / "src" / "compiler" / "lainc.exe"
    if exe.exists():
        return exe
    return ROOT / "src" / "compiler" / "lainc"


def run_case(exe: pathlib.Path, source: pathlib.Path) -> bool:
    expected_path = source.with_suffix(".ast")
    result = subprocess.run([str(exe), str(source)], capture_output=True, text=True)
    if result.returncode != 0:
        print(f"FAIL {source.name}: ast_dump failed")
        print(result.stderr)
        return False

    actual = normalize(result.stdout)
    expected = normalize(expected_path.read_text(encoding="utf-8"))
    if actual != expected:
        print(f"FAIL {source.name}: golden mismatch")
        diff = difflib.unified_diff(
            expected.splitlines(),
            actual.splitlines(),
            fromfile=str(expected_path),
            tofile="actual",
            lineterm="",
        )
        print("\n".join(diff))
        return False

    forbidden = ("(call", "type-app", "effect-name", "raw.node", "middle")
    hit = next((word for word in forbidden if word in actual), None)
    if hit:
        print(f"FAIL {source.name}: forbidden semantic marker {hit}")
        return False

    print(f"PASS {source.name}")
    return True


def run_lainc_emit_ast_case(compiler: pathlib.Path, source: pathlib.Path) -> bool:
    expected_path = source.with_suffix(".ast")
    out_path = BUILD / f"{source.stem}.lainc.ast"
    result = subprocess.run(
        [str(compiler), "--emit-ast", str(source), str(out_path)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"FAIL {source.name}: lainc --emit-ast failed")
        print(result.stderr or result.stdout)
        return False
    if not out_path.exists():
        print(f"FAIL {source.name}: lainc --emit-ast did not write output")
        return False

    actual = normalize(out_path.read_text(encoding="utf-8"))
    expected = normalize(expected_path.read_text(encoding="utf-8"))
    if actual != expected:
        print(f"FAIL {source.name}: lainc --emit-ast golden mismatch")
        diff = difflib.unified_diff(
            expected.splitlines(),
            actual.splitlines(),
            fromfile=str(expected_path),
            tofile=str(out_path),
            lineterm="",
        )
        print("\n".join(diff))
        return False

    print(f"PASS lainc --emit-ast {source.name}")
    return True


def main() -> int:
    try:
        exe = build_dump_tool()
    except RuntimeError as exc:
        print(f"FAIL build ast_dump: {exc}")
        return 1

    compiler = compiler_path()
    if not compiler.exists():
        print(f"FAIL compiler missing: {compiler}")
        return 1

    sources = sorted(FIXTURES.glob("*.lain"))
    if not sources:
        print("FAIL no AST fixtures")
        return 1

    failed = 0
    for source in sources:
        if not run_case(exe, source):
            failed += 1
        if not run_lainc_emit_ast_case(compiler, source):
            failed += 1

    total = len(sources) * 2
    print(f"\nAST golden: {total - failed}/{total} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
