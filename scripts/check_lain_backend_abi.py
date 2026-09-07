#!/usr/bin/env python3
"""Check the Lain-written backend against capability ABI v1.

The check is intentionally independent of the compiler.  During migration it
provides a deterministic inventory of legacy ``@foreign`` declarations; once
the source is migrated, the same command becomes a zero-legacy gate.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BACKEND = ROOT / "src" / "lainc" / "backend_c.lain"
EXPECTED = {
    "bootstrap.allocate-pages": "backend.allocate",
    "bootstrap.artifact-begin": "backend.artifact_begin",
    "bootstrap.artifact-write-byte": "backend.artifact_write_byte",
    "bootstrap.artifact-finish": "backend.artifact_finish",
    "bootstrap.source-length": "backend.source_length",
    "bootstrap.source-data": "backend.source_data",
    "bootstrap.copy-bytes": "backend.copy_bytes",
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
                "legacy_link_name": link_name,
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
    unknown = [entry for entry in entries if not entry["logical_name"]]
    if unknown:
        names = ", ".join(entry["legacy_link_name"] for entry in unknown)
        print(f"unknown legacy backend capabilities: {names}", file=sys.stderr)
        return 1
    if entries:
        names = ", ".join(entry["legacy_link_name"] for entry in entries)
        print(
            "legacy backend @foreign declarations remain: " + names,
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
