#!/usr/bin/env python3
"""Structural gate for the concrete main-branch compiler entry."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
ENTRY = ROOT / "packages" / "lain" / "compiler" / "lainc.lain"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    text = ENTRY.read_text(encoding="utf-8")
    require("compiler_core.Compiler(Memory)" in text, "compiler core is not instantiated")
    require("memory_model.Model(" in text, "memory model is not instantiated")
    require("let compiler_compile = std::func(" in text, "compiler entry is missing")
    require("Compiler.compile(request)" in text, "entry does not call compiler core")
    require("stage1_compiler" not in text, "main entry depends on transitional compiler")
    require("@foreign" not in text, "platform ABI leaked into compiler core entry")
    print("main compiler entry contract: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
