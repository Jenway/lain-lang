#!/usr/bin/env python3
"""Contract checks for the native lainc driver and reproducible build flags."""

from __future__ import annotations

import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[3]


def main() -> int:
    builder = (ROOT / "scripts" / "build_lainc_native.py").read_text(
        encoding="utf-8"
    )
    driver = (ROOT / "seed" / "src" / "host" / "native_lainc.c").read_text(
        encoding="utf-8"
    )
    source_compiler = (ROOT / "src" / "lainc" / "lainc.lain").read_text(
        encoding="utf-8"
    )
    backend = (ROOT / "src" / "lainc" / "backend_c.lain").read_text(
        encoding="utf-8"
    )
    required_builder = (
        "lainir-print",
        "run(CHECK, compiler_l1, \"compiler_compile\")",
        '"zig", "cc", "-std=c11", "-O2"',
        '"-DLAIN_NATIVE_LIBRARY_ENTRY"',
        '"seed" / "src" / "interpreter" / "interpreter.c"',
    )
    for spelling in required_builder:
        if spelling not in builder:
            raise RuntimeError(f"native build contract missing: {spelling}")
    for spelling in (
        'usage: lainc.exe [--run] -o <output.l1> <source.lain> [source.lain ...]',
        'run_emitted_artifact(artifact_path)',
        'lainir_parse_module_checked',
        "bootstrap_artifact_finish();",
        "free(sources[index].data);",
        'static NativeRunContext compile_context;',
        'allocation->next = compile_context.allocations;',
        'void bootstrap_release_pages(uintptr_t pointer)',
        'native_run_release_allocations(&compile_context);',
    ):
        if spelling not in driver:
            raise RuntimeError(f"native driver contract missing: {spelling}")
    for spelling in (
        "String literals are NUL-terminated",
        "let actual_length: usize = 0;",
        "while sb_load8_at(literal, actual_length) != 0",
    ):
        if spelling not in source_compiler:
            raise RuntimeError(f"source compiler literal contract missing: {spelling}")
    if "append_span(output, source, name_cursor, name_end);" not in backend:
        raise RuntimeError(
            "backend prototype emitter must pass append_span an absolute end"
        )
    if "name_end - name_cursor" in backend:
        raise RuntimeError(
            "backend prototype emitter must not pass a span length as its end"
        )
    print("native lainc driver contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
