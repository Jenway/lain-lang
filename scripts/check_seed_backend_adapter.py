#!/usr/bin/env python3
"""Check that the seed/native host exposes every backend ABI binding."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HOST_SOURCES = (
    ROOT / "seed" / "src" / "host" / "bootstrap.c",
    ROOT / "seed" / "src" / "host" / "native_lainc.c",
    ROOT / "seed" / "src" / "cli" / "seed_main.c",
)
LINKS = {
    "backend.allocate": "backend.allocate",
    "backend.artifact_begin": "backend.artifact_begin",
    "backend.artifact_write_byte": "backend.artifact_write_byte",
    "backend.artifact_finish": "backend.artifact_finish",
    "backend.source_count": "backend.source_count",
    "backend.source_length": "backend.source_length",
    "backend.source_data": "backend.source_data",
    "backend.copy_bytes": "backend.copy_bytes",
}


def main() -> int:
    text = "\n".join(path.read_text(encoding="utf-8") for path in HOST_SOURCES)
    missing = [
        f"{logical} -> {link}"
        for logical, link in LINKS.items()
        if not re.search(re.escape(link), text)
    ]
    if missing:
        for item in missing:
            print(f"FAIL missing seed backend binding: {item}")
        return 1
    print("PASS seed backend adapter exposes all ABI bindings")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
