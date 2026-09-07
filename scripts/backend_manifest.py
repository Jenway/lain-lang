"""Build a deterministic link/capability manifest from canonical LAIN-IR."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path


_EXTERN = re.compile(r"^#extern\s+#proc\s+([^ (]+)\((.*)\)\s+->\s+(.+);$")


def build_manifest(text: str, *, source: str = "") -> dict[str, object]:
    externs: list[dict[str, str]] = []
    for raw_line in text.replace("\r\n", "\n").replace("\r", "\n").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        match = _EXTERN.match(line)
        if not match:
            continue
        symbol, parameters, result = match.groups()
        if symbol.startswith(("bootstrap.", "backend.")):
            capability = symbol
        else:
            capability = "foreign"
        externs.append(
            {
                "symbol": symbol,
                "parameters": parameters,
                "result": result,
                "capability": capability,
            }
        )
    externs.sort(key=lambda item: (item["symbol"], item["parameters"], item["result"]))
    payload: dict[str, object] = {
        "schema": "lain.backend.link-manifest.v1",
        "source": source,
        "source_sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        "externs": externs,
        "capabilities": sorted({item["capability"] for item in externs}),
    }
    return payload


def write_manifest(input_path: Path, output_path: Path) -> dict[str, object]:
    text = input_path.read_text(encoding="utf-8")
    # Keep manifests byte-for-byte stable when the same relative input is
    # processed on Windows and POSIX hosts.
    source = str(input_path).replace("\\", "/")
    payload = build_manifest(text, source=source)
    output_path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return payload
