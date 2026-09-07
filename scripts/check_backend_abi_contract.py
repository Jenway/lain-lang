#!/usr/bin/env python3
"""Keep BackendShape, ABI documentation, and source inventory in sync."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "src" / "lainir" / "api_contract.lain"
DOC = ROOT / "docs" / "implementation" / "lain-backend-capability-abi.md"
INVENTORY = ROOT / "scripts" / "check_lain_backend_abi.py"
NAMES = (
    "source_count",
    "source_data",
    "source_length",
    "allocate",
    "copy_bytes",
    "artifact_begin",
    "artifact_write_byte",
    "artifact_finish",
)
INVENTORY_NAMES = NAMES


def main() -> int:
    texts = {
        "contract": CONTRACT.read_text(encoding="utf-8"),
        "documentation": DOC.read_text(encoding="utf-8"),
        "inventory": INVENTORY.read_text(encoding="utf-8"),
    }
    failures: list[str] = []
    for label, text in texts.items():
        names = INVENTORY_NAMES if label == "inventory" else NAMES
        for name in names:
            if not re.search(rf"\b{re.escape(name)}\b", text):
                failures.append(f"{label} is missing backend capability {name}")
    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        return 1
    print("PASS backend ABI contract consistency")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
