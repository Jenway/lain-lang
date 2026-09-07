#!/usr/bin/env python3
"""Check the Lain-written backend against capability ABI v1."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BACKEND = ROOT / "src" / "lainc" / "backend_c.lain"
EXPECTED = {
    "backend.allocate": "backend.allocate",
    "backend.artifact_begin": "backend.artifact_begin",
    "backend.artifact_write_byte": "backend.artifact_write_byte",
    "backend.artifact_finish": "backend.artifact_finish",
    "backend.source_length": "backend.source_length",
    "backend.source_data": "backend.source_data",
    "backend.source_count": "backend.source_count",
    "backend.copy_bytes": "backend.copy_bytes",
}
FOREIGN = re.compile(
    r'@foreign\(c,\s*link_name\s*=\s*"([^"]+)"\)\s*\n'
    r'let\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*std::func\((.*?)\)\s*'
    r'->\s*([A-Za-z0-9_]+)\s*;',
    re.DOTALL,
)


def inventory() -> list[dict[str, str]]:
    text = BACKEND.read_text(encoding="utf-8")
    result = []
    for match in FOREIGN.finditer(text):
        link_name, binding, parameters, result_type = match.groups()
        result.append(
            {
                "binding": binding,
                "link_name": link_name,
                "logical_name": EXPECTED.get(link_name, ""),
                "parameters": " ".join(parameters.split()),
                "result": result_type,
            }
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", action="store_true", help="print JSON inventory")
    args = parser.parse_args()
    entries = inventory()
    if args.report:
        print(json.dumps(entries, indent=2, sort_keys=True))
    names = {entry["link_name"] for entry in entries}
    missing = sorted(set(EXPECTED) - names)
    unexpected = sorted(names - set(EXPECTED))
    if missing or unexpected or len(entries) != len(EXPECTED):
        detail = []
        if missing:
            detail.append("missing " + ", ".join(missing))
        if unexpected:
            detail.append("unexpected " + ", ".join(unexpected))
        if len(entries) != len(EXPECTED):
            detail.append(f"expected {len(EXPECTED)} declarations, found {len(entries)}")
        print("backend capability ABI mismatch: " + "; ".join(detail), file=sys.stderr)
        return 1
    print("PASS backend capability ABI v1 source declarations")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
