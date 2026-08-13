#!/usr/bin/env python3
"""Structural gate for the concrete main-branch compiler entry."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
ENTRY = ROOT / "src" / "compiler" / "lainc.lain"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    text = ENTRY.read_text(encoding="utf-8")
    require("compiler_api.compiler_api.API" in text, "thin compiler API is not exported")
    require("let lainc: Module = std::module" in text, "lainc module is missing")
    require("compiler_core.Compiler(Memory)" not in text, "lainc eagerly specializes compiler core")
    require("memory_model.Model(" not in text, "lainc chooses a memory policy")
    require("compiler_compile" not in text, "thin entry owns no concrete compile function")
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
