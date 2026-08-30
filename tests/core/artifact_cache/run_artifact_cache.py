#!/usr/bin/env python3
"""Focused tests for the bootstrap artifact cache trust boundary."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/core/self_hosting"))

from artifact_cache import (  # noqa: E402
    SpecializationCache,
    cache_matches,
    input_fingerprint,
    specialization_key,
    write_stamp,
)


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

        # Specialization keys are canonical across mapping order and process
        # boundaries, while changing target/backend components invalidates the
        # artifact in the on-disk cache.
        components = {
            "source_id": "module::vec",
            "type_argument": "i32",
            "value_argument": 0,
            "policy": "checked",
            "environment": "env-a",
            "target": "x86_64-windows",
            "backend": "lain-backend-v1",
        }
        key = specialization_key("specialization-v1", components)
        reordered = specialization_key(
            "specialization-v1",
            {name: components[name] for name in reversed(list(components))},
        )
        require(key == reordered, "specialization key depends on mapping order")
        child = subprocess.run(
            [
                sys.executable,
                "-c",
                "import json,sys; from artifact_cache import specialization_key; "
                "print(specialization_key(sys.argv[1], json.loads(sys.argv[2])))",
                "specialization-v1",
                json.dumps(components),
            ],
            cwd=Path(__file__).resolve().parents[1] / "self_hosting",
            capture_output=True,
            text=True,
            check=True,
        )
        require(child.stdout.strip() == key,
                "specialization key changed across process boundary")
        cache = SpecializationCache(directory / "specializations")
        require(cache.get(key) is None, "empty specialization cache produced a hit")
        cache.put(key, b"specialized artifact\n")
        require(cache.get(key) == b"specialized artifact\n",
                "stored specialization artifact did not hit")
        changed = dict(components)
        changed["target"] = "aarch64-windows"
        changed_key = specialization_key("specialization-v1", changed)
        require(cache.get(changed_key) is None,
                "target change produced a specialization cache hit")
        cache_output = directory / "specializations" / f"{key}.l1"
        cache_output.write_bytes(b"tampered\n")
        require(cache.get(key) is None,
                "tampered specialization artifact produced a cache hit")

    print("artifact cache trust contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
