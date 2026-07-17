#!/usr/bin/env python3
"""Focused tests for the bootstrap artifact cache trust boundary."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/core/self_hosting"))

from artifact_cache import cache_matches, input_fingerprint, write_stamp  # noqa: E402


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    schema = "focused-cache-v1"
    with tempfile.TemporaryDirectory(prefix="lain-artifact-cache-") as raw:
        directory = Path(raw)
        output = directory / "compiler.l1"
        stamp = directory / "compiler.stamp.json"
        output.write_bytes(b"valid artifact\n")

        original = input_fingerprint(
            schema,
            [
                ("builder", b"builder-v1"),
                ("canonical-source", b"let main = fn() { 42 }"),
                ("stage1", b"stage1-v1"),
                ("tool", b"tool-v1"),
            ],
        )
        write_stamp(output, stamp, schema, original)
        require(cache_matches(output, stamp, schema, original),
                "fresh cache entry did not hit")

        changed_source = input_fingerprint(
            schema,
            [
                ("builder", b"builder-v1"),
                ("canonical-source", b"let main = fn() { 43 }"),
                ("stage1", b"stage1-v1"),
                ("tool", b"tool-v1"),
            ],
        )
        require(not cache_matches(output, stamp, schema, changed_source),
                "canonical source content change produced a cache hit")

        changed_stage1 = input_fingerprint(
            schema,
            [
                ("builder", b"builder-v1"),
                ("canonical-source", b"let main = fn() { 42 }"),
                ("stage1", b"stage1-v2"),
                ("tool", b"tool-v1"),
            ],
        )
        require(not cache_matches(output, stamp, schema, changed_stage1),
                "stage1 artifact content change produced a cache hit")

        output.write_bytes(b"tampered artifact\n")
        require(not cache_matches(output, stamp, schema, original),
                "tampered output produced a cache hit")

        output.write_bytes(b"valid artifact\n")
        write_stamp(output, stamp, schema, original)
        data = json.loads(stamp.read_text(encoding="utf-8"))
        data["output"] = "0" * 64
        stamp.write_text(json.dumps(data), encoding="utf-8")
        require(not cache_matches(output, stamp, schema, original),
                "tampered stamp output hash produced a hit")

        write_stamp(output, stamp, schema, original)
        data = json.loads(stamp.read_text(encoding="utf-8"))
        data["schema"] = "focused-cache-v0"
        stamp.write_text(json.dumps(data), encoding="utf-8")
        require(not cache_matches(output, stamp, schema, original),
                "wrong cache schema produced a hit")

        stamp.write_text("not json", encoding="utf-8")
        require(not cache_matches(output, stamp, schema, original),
                "malformed stamp produced a hit")

    print("artifact cache trust contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
