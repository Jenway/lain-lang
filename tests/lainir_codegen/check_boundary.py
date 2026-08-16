#!/usr/bin/env python3
"""Keep the self-hosted compiler free of a hidden native implementation."""

from __future__ import annotations

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
IMPLEMENTATION = ROOT / "src" / "lainir"
NATIVE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".a", ".lib"}


def main() -> int:
    violations = [
        path.relative_to(ROOT)
        for path in IMPLEMENTATION.rglob("*")
        if path.is_file() and path.suffix.lower() in NATIVE_SUFFIXES
    ]
    if violations:
        print(
            "src/lainir must not contain a hidden native implementation:",
            file=sys.stderr,
        )
        for path in violations:
            print(f"  {path}", file=sys.stderr)
        return 1

    build_files = [ROOT / "seed" / "build.zig"]
    for build_file in build_files:
        text = build_file.read_text(encoding="utf-8")
        if "src/lainir" in text.replace("\\", "/"):
            print(
                f"{build_file.relative_to(ROOT)} must not compile src/lainir "
                "as a native helper library",
                file=sys.stderr,
            )
            return 1

    print("LAIN-IR compiler boundary: no hidden native implementation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

