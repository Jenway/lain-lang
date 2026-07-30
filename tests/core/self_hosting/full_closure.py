#!/usr/bin/env python3
"""Materialize the complete compiler source closure in deterministic order."""

from __future__ import annotations

import re
from pathlib import Path

from compiler_source import MODULES, ROOT


OUT = ROOT / "build/core-self-hosting/full_compiler_source.lain"
MANIFEST = ROOT / "build/core-self-hosting/full_compiler_sources.txt"
IMPORT = re.compile(r'import\("([^"]+)"\)')


def import_path(name: str) -> Path:
    parts = name.split("::")
    if parts[0] == "std":
        return ROOT.joinpath("std", *parts[1:]).with_suffix(".lain")
    if parts[0] == "packages":
        return ROOT.joinpath(*parts).with_suffix(".lain")
    raise ValueError(f"unsupported import root in {name!r}")


def visit(path: Path, seen: set[Path], ordered: list[Path]) -> None:
    path = path.resolve()
    if path in seen:
        return
    if not path.is_file():
        raise FileNotFoundError(path)
    seen.add(path)
    text = path.read_text(encoding="utf-8")
    for imported in IMPORT.findall(text):
        visit(import_path(imported), seen, ordered)
    ordered.append(path)


def build_full_closure() -> tuple[Path, tuple[Path, ...]]:
    seen: set[Path] = set()
    ordered: list[Path] = []
    for relative in MODULES:
        visit(ROOT / relative, seen, ordered)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    sections = []
    for path in ordered:
        relative = path.relative_to(ROOT).as_posix()
        sections.append(
            f"// -- source: {relative} --\n"
            + path.read_text(encoding="utf-8").rstrip("\r\n")
            + "\n"
        )
    OUT.write_text("\n".join(sections), encoding="utf-8", newline="\n")
    MANIFEST.write_text(
        "".join(f"{path.relative_to(ROOT).as_posix()}\n" for path in ordered),
        encoding="utf-8",
        newline="\n",
    )
    return OUT, tuple(ordered)


if __name__ == "__main__":
    output, sources = build_full_closure()
    print(f"{output.relative_to(ROOT)} ({len(sources)} sources)")
