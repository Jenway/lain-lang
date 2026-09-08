#!/usr/bin/env python3
"""Verify a frozen lainc artifact and its source-closure snapshot manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from lainc_sources import composed_compiler_sources


ROOT = Path(__file__).resolve().parents[1]


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def source_closure_hash() -> str:
    digest = hashlib.sha256()
    for path in composed_compiler_sources(ROOT):
        digest.update(path.relative_to(ROOT).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("artifact", type=Path)
    parser.add_argument("manifest", type=Path, nargs="?")
    args = parser.parse_args()
    artifact = args.artifact if args.artifact.is_absolute() else ROOT / args.artifact
    manifest = args.manifest or artifact.with_suffix(artifact.suffix + ".snapshot.json")
    if not manifest.is_absolute():
        manifest = ROOT / manifest
    if not artifact.is_file() or not manifest.is_file():
        raise SystemExit("snapshot artifact or manifest is missing")
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    if payload.get("schema") != "lain-bootstrap-snapshot-v1":
        raise SystemExit("unsupported bootstrap snapshot schema")
    if payload.get("abi_version") != 1:
        raise SystemExit("unsupported bootstrap snapshot ABI version")
    data = artifact.read_bytes()
    if payload.get("artifact_bytes") != len(data):
        raise SystemExit("bootstrap snapshot artifact length mismatch")
    if payload.get("artifact_sha256") != sha256_bytes(data):
        raise SystemExit("bootstrap snapshot artifact hash mismatch")
    if payload.get("source_closure_sha256") != source_closure_hash():
        raise SystemExit("bootstrap snapshot source closure hash mismatch")
    print(
        "PASS bootstrap snapshot "
        f"(abi={payload['abi_version']}, artifact_sha256={payload['artifact_sha256']})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
