#!/usr/bin/env python3
"""Check the verified stamp contract used by Lain artifact builders."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
import scripts.build_lain_compiler as builder


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lain-build-cache-") as directory:
        root = Path(directory)
        output = root / "artifact.l1"
        stamp = root / "artifact.stamp.json"
        artifact = b"#proc main() -> #bits<32> {\n  #return 42\n}\n"
        output.write_bytes(artifact)
        fingerprint = "input-fingerprint"
        stamp.write_text(
            json.dumps(
                {
                    "schema": builder.SCHEMA,
                    "inputs": fingerprint,
                    "output": hashlib.sha256(artifact).hexdigest(),
                }
            ),
            encoding="utf-8",
        )
        old_output, old_stamp = builder.OUTPUT, builder.STAMP
        try:
            builder.OUTPUT, builder.STAMP = output, stamp
            if not builder.cache_valid(fingerprint):
                raise RuntimeError("valid build stamp was rejected")
            output.write_bytes(artifact + b"#tampered\n")
            if builder.cache_valid(fingerprint):
                raise RuntimeError("tampered artifact was accepted")
        finally:
            builder.OUTPUT, builder.STAMP = old_output, old_stamp
    print("build artifact cache contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
